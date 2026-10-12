#!/usr/bin/env python3
"""Offline 3-batch Chinese lexicon import & polyphonic verification pipeline.

Reads the normalized 8-column annotated TSV tables and 6-column pending TSV
tables from the offline lexicon research bundle, cross-validates readings and
frequencies against system LibIME sc.dict, ModernIME baseline knowledge, and
hotwords, and generates:
  - data/pinyin/modernime-knowledge.tsv (6-column normalized knowledge table)
  - data/pinyin/modernime-knowledge.raw (3-column LibIME raw dictionary input)
  - data/pinyin/POLYPHONIC_REVIEW.tsv (10-column quarantined polyphonic manifest)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

from generate_pinyin_knowledge import (
    CHINESE_RE,
    PINYIN_RE,
    abbreviation_for,
    normalize_phrase,
    normalize_syllable,
    pinyin_from_source,
)

STRONG_POLYPHONIC_CHARS = set(
    "重行长乐率单曾都藏传调弹盛厦臭给没还着了得地的为和与参便处觉角模省宿血殷载占转幢作"
    "背薄泊称乘冲创答大担当倒斗度恶发坊分缝杆干葛更供冠观龟哈汗巷号喝横哄糊划晃会奇几济纪系"
    "假间将降教结解尽禁劲卷看壳空累量露落埋脉蔓蒙磨难泥宁弄排迫曝卡强悄翘切曲圈散丧扫色塞煞"
    "扇舍什识似数说遂沓踏帖吐拓瓦委吓鲜相兴畜旋削咽淹燕要叶应佣吁晕择扎轧粘涨正只中种轴属爪"
    "撞综钻柞椎伺沌勒茄艾蚌堡暴辟扁骠屏埔曝稽隽槛缴矜龟巷吭烙拎笼络抹蔓氓泌缪泥拗排胖劈缥"
    "仆朴瀑栖蹊稽荨茜强襁翘鞘亲黥曲雀妊娠塞臊色刹杉苫裳少召折锗甄帧殖峙中种轴嘱著拽篆兹综"
)

EXPLICIT_POLYPHONIC_FIXES: dict[str, str] = {
    "弹出窗口": "tan'chu'chuang'kou",
    "弹出菜单": "tan'chu'cai'dan",
    "弹出框": "tan'chu'kuang",
    "弹出式": "tan'chu'shi",
    "调优": "tiao'you",
    "性能调优": "xing'neng'tiao'you",
    "参数调优": "can'shu'tiao'you",
    "多重继承": "duo'chong'ji'cheng",
    "多重背包": "duo'chong'bei'bao",
    "多重签名": "duo'chong'qian'ming",
    "多重线性回归": "duo'chong'xian'xing'hui'gui",
    "方法重载": "fang'fa'chong'zai",
    "方法重写": "fang'fa'chong'xie",
    "函数重载": "han'shu'chong'zai",
    "运算符重载": "yun'suan'fu'chong'zai",
    "暴力解法": "bao'li'jie'fa",
    "变长参数": "bian'chang'can'shu",
    "变长数组": "bian'chang'shu'zu",
    "变长编码": "bian'chang'bian'ma",
    "椎间盘": "zhui'jian'pan",
    "椎间盘突出": "zhui'jian'pan'tu'chu",
    "腰椎间盘突出": "yao'zhui'jian'pan'tu'chu",
    "颈椎病": "jing'zhui'bing",
    "脊椎动物": "ji'zhui'dong'wu",
    "伺服电机": "si'fu'dian'ji",
    "伺服系统": "si'fu'xi'tong",
    "鼻塞": "bi'sai",
    "川芎": "chuan'xiong",
    "川芎嗪": "chuan'xiong'qin",
    "白参": "bai'shen",
    "刺参": "ci'shen",
    "红参": "hong'shen",
    "丹参": "dan'shen",
    "党参": "dang'shen",
    "西洋参": "xi'yang'shen",
    "高丽参": "gao'li'shen",
    "颠茄": "dian'qie",
    "炒茄丝": "chao'qie'si",
    "番茄酱": "fan'qie'jiang",
    "藏医": "zang'yi",
    "藏药": "zang'yao",
    "藏传": "zang'chuan",
    "藏传佛教": "zang'chuan'fo'jiao",
    "藏狐": "zang'hu",
    "藏羚羊": "zang'ling'yang",
    "藏族": "zang'zu",
    "青藏高原": "qing'zang'gao'yuan",
    "昌都": "chang'du",
    "昌都市": "chang'du'shi",
    "昌都地区": "chang'du'di'qu",
    "都柏林": "du'bo'lin",
    "都匀": "du'yun",
    "都匀市": "du'yun'shi",
    "丰都": "feng'du",
    "丰都县": "feng'du'xian",
    "都安县": "du'an'xian",
    "都江堰": "du'jiang'yan",
    "宁都县": "ning'du'xian",
    "于都县": "yu'du'xian",
    "曾荫权": "zeng'yin'quan",
    "曾子": "zeng'zi",
    "曾国藩": "zeng'guo'fan",
    "曾巩": "zeng'gong",
    "曾侯乙": "zeng'hou'yi",
    "单雄信": "shan'xiong'xin",
    "单县": "shan'xian",
    "单县羊肉汤": "shan'xian'yang'rou'tang",
    "单于": "chan'yu",
    "沌口": "zhuan'kou",
    "沌口开发区": "zhuan'kou'kai'fa'qu",
    "多普勒": "duo'pu'le",
    "多普勒效应": "duo'pu'le'xiao'ying",
    "费德勒": "fei'de'le",
    "戴姆勒": "dai'mu'le",
    "俄勒冈": "e'le'gang",
    "俄勒冈州": "e'le'gang'zhou",
    "开普勒": "kai'pu'le",
    "希特勒": "xi'te'le",
    "泰勒公式": "tai'le'gong'shi",
    "泰勒展开": "tai'le'zhan'kai",
}

CURATED_SUPPLEMENTS: list[tuple[str, str, int, str, str]] = [
    ('阿炳', "a'bing", 2600, 'person', 'curated-person'),
    ('阿尔茨海默病', "a'er'ci'hai'mo'bing", 4200, 'medical', 'curated-medical'),
    ('阿伏伽德罗', "a'fu'jia'de'luo", 2200, 'person', 'curated-person'),
    ('阿基米德', "a'ji'mi'de", 3200, 'person', 'curated-person'),
    ('阿来', "a'lai", 2200, 'person', 'curated-person'),
    ('埃索美拉唑', "ai'suo'mei'la'zuo", 4100, 'medical', 'curated-medical'),
    ('按份责任', "an'fen'ze'ren", 3400, 'law', 'curated-law'),
    ('奥美拉唑', "ao'mei'la'zuo", 4400, 'medical', 'curated-medical'),
    ('熬夜加班', "ao'ye'jia'ban", 3900, 'daily', 'curated-daily'),
    ('拔草', "ba'cao", 3400, 'daily', 'curated-daily'),
    ('巴尔扎克', "ba'er'zha'ke", 3100, 'person', 'curated-person'),
    ('巴斯德', "ba'si'de", 2500, 'person', 'curated-person'),
    ('靶向治疗', "ba'xiang'zhi'liao", 4400, 'medical', 'curated-medical'),
    ('百白破疫苗', "bai'bai'po'yi'miao", 3900, 'medical', 'curated-medical'),
    ('白春礼', "bai'chun'li", 2600, 'person', 'curated-person'),
    ('白居易', "bai'ju'yi", 4200, 'person', 'curated-person'),
    ('拜伦', "bai'lun", 2800, 'person', 'curated-person'),
    ('拜占庭容错', "bai'zhan'ting'rong'cuo", 3500, 'it', 'curated-it'),
    ('班固', "ban'gu", 2100, 'person', 'curated-person'),
    ('半精度浮点', "ban'jing'du'fu'dian", 3500, 'it', 'curated-it'),
    ('半同步复制', "ban'tong'bu'fu'zhi", 3300, 'it', 'curated-it'),
    ('班昭', "ban'zhao", 2100, 'person', 'curated-person'),
    ('鲍林', "bao'lin", 2100, 'person', 'curated-person'),
    ('保密协议', "bao'mi'xie'yi", 3600, 'daily', 'curated-daily'),
    ('鲍明远', "bao'ming'yuan", 840, 'person', 'curated-person'),
    ('报销单', "bao'xiao'dan", 3400, 'daily', 'curated-daily'),
    ('报销流程', "bao'xiao'liu'cheng", 3300, 'daily', 'curated-daily'),
    ('保研', "bao'yan", 3800, 'daily', 'curated-daily'),
    ('包在我身上', "bao'zai'wo'shen'shang", 2800, 'daily', 'curated-daily'),
    ('包拯', "bao'zheng", 2500, 'person', 'curated-person'),
    ('北岛', "bei'dao", 2900, 'person', 'curated-person'),
    ('悲观并发控制', "bei'guan'bing'fa'kong'zhi", 3400, 'it', 'curated-it'),
    ('北交所', "bei'jiao'suo", 4300, 'finance', 'curated-finance'),
    ('贝克勒尔', "bei'ke'le'er", 1900, 'person', 'curated-person'),
    ('贝时璋', "bei'shi'zhang", 1350, 'person', 'curated-person'),
    ('备选方案', "bei'xuan'fang'an", 3600, 'daily', 'curated-daily'),
    ('毕达哥拉斯', "bi'da'ge'la'si", 2800, 'person', 'curated-person'),
    ('避雷', "bi'lei", 3700, 'daily', 'curated-daily'),
    ('毕业论文', "bi'ye'lun'wen", 4300, 'daily', 'curated-daily'),
    ('便常规', "bian'chang'gui", 3800, 'medical', 'curated-medical'),
    ('变分自编码器', "bian'fen'zi'bian'ma'qi", 2900, 'it', 'curated-it'),
    ('卞之琳', "bian'zhi'lin", 1550, 'person', 'curated-person'),
    ('表见代理', "biao'jian'dai'li", 3800, 'law', 'curated-law'),
    ('表面抗原', "biao'mian'kang'yuan", 3800, 'medical', 'curated-medical'),
    ('表皮生长因子受体', "biao'pi'sheng'zhang'yin'zi'shou'ti", 3600, 'medical', 'curated-medical'),
    ('别担心', "bie'dan'xin", 3800, 'daily', 'curated-daily'),
    ('别见外', "bie'jian'wai", 2700, 'daily', 'curated-daily'),
    ('别介意', "bie'jie'yi", 3100, 'daily', 'curated-daily'),
    ('别着急', "bie'zhao'ji", 3600, 'daily', 'curated-daily'),
    ('并不觉得', "bing'bu'jue'de", 2600, 'daily', 'curated-daily'),
    ('病态窦房结综合征', "bing'tai'dou'fang'jie'zong'he'zheng", 3300, 'medical', 'curated-medical'),
    ('秉志', "bing'zhi", 1100, 'person', 'curated-person'),
    ('拨备覆盖率', "bo'bei'fu'gai'lv", 3700, 'finance', 'curated-finance'),
    ('玻尔', "bo'er", 2800, 'person', 'curated-person'),
    ('博尔赫斯', "bo'er'he'si", 3200, 'person', 'curated-person'),
    ('玻尔兹曼', "bo'er'zi'man", 2200, 'person', 'curated-person'),
    ('伯努利', "bo'nu'li", 2600, 'person', 'curated-person'),
    ('泊松', "bo'song", 2400, 'person', 'curated-person'),
    ('不安抗辩权', "bu'an'kang'bian'quan", 3700, 'law', 'curated-law'),
    ('不出所料', "bu'chu'suo'liao", 3800, 'daily', 'curated-daily'),
    ('不动产投资信托基金', "bu'dong'chan'tou'zi'xin'tuo'ji'jin", 3700, 'finance', 'curated-finance'),
    ('不好意思', "bu'hao'yi'si", 4500, 'daily', 'curated-daily'),
    ('不见不散', "bu'jian'bu'san", 3100, 'daily', 'curated-daily'),
    ('补觉', "bu'jiao", 2900, 'daily', 'curated-daily'),
    ('布莱克舒尔斯模型', "bu'lai'ke'shu'er'si'mo'xing", 3700, 'finance', 'curated-finance'),
    ('布隆过滤器', "bu'long'guo'lv'qi", 3900, 'it', 'curated-it'),
    ('布鲁氏菌病', "bu'lu'shi'jun'bing", 3300, 'medical', 'curated-medical'),
    ('不稳定型心绞痛', "bu'wen'ding'xing'xin'jiao'tong", 3600, 'medical', 'curated-medical'),
    ('不值一提', "bu'zhi'yi'ti", 2800, 'daily', 'curated-daily'),
    ('不至于', "bu'zhi'yu", 4100, 'daily', 'curated-daily'),
    ('踩坑', "cai'keng", 3900, 'daily', 'curated-daily'),
    ('踩雷', "cai'lei", 3600, 'daily', 'curated-daily'),
    ('蔡文姬', "cai'wen'ji", 3200, 'person', 'curated-person'),
    ('蔡襄', "cai'xiang", 2200, 'person', 'curated-person'),
    ('蔡元培', "cai'yuan'pei", 3200, 'person', 'curated-person'),
    ('蔡泽', "cai'ze", 620, 'person', 'curated-person'),
    ('残差网络', "can'cha'wang'luo", 3800, 'it', 'curated-it'),
    ('曹孟德', "cao'meng'de", 2400, 'person', 'curated-person'),
    ('曹子建', "cao'zi'jian", 1600, 'person', 'curated-person'),
    ('岑参', "cen'shen", 1800, 'person', 'curated-person'),
    ('层归一化', "ceng'gui'yi'hua", 3500, 'it', 'curated-it'),
    ('差不多', "cha'bu'duo", 4100, 'daily', 'curated-daily'),
    ('差不多行了', "cha'bu'duo'xing'liao", 2700, 'daily', 'curated-daily'),
    ('查重率', "cha'chong'lv", 3900, 'daily', 'curated-daily'),
    ('拆快递', "chai'kuai'di", 3900, 'daily', 'curated-daily'),
    ('差旅费', "chai'lv'fei", 3200, 'daily', 'curated-daily'),
    ('长短期记忆网络', "chang'duan'qi'ji'yi'wang'luo", 3700, 'it', 'curated-it'),
    ('长话短说', "chang'hua'duan'shuo", 3600, 'daily', 'curated-daily'),
    ('长期股权投资', "chang'qi'gu'quan'tou'zi", 3800, 'finance', 'curated-finance'),
    ('常香玉', "chang'xiang'yu", 2100, 'person', 'curated-person'),
    ('场效应晶体管', "chang'xiao'ying'jing'ti'guan", 3800, 'it', 'curated-it'),
    ('肠易激综合征', "chang'yi'ji'zong'he'zheng", 3800, 'medical', 'curated-medical'),
    ('晁补之', "chao'bu'zhi", 1450, 'person', 'curated-person'),
    ('晁错', "chao'cuo", 1650, 'person', 'curated-person'),
    ('抄送邮件', "chao'song'you'jian", 3500, 'daily', 'curated-daily'),
    ('陈芳允', "chen'fang'yun", 1350, 'person', 'curated-person'),
    ('陈化成', "chen'hua'cheng", 980, 'person', 'curated-person'),
    ('陈寅恪', "chen'yin'ke", 3700, 'person', 'curated-person'),
    ('陈忠实', "chen'zhong'shi", 3100, 'person', 'curated-person'),
    ('陈子昂', "chen'zi'ang", 1800, 'person', 'curated-person'),
    ('程不识', "cheng'bu'shi", 630, 'person', 'curated-person'),
    ('惩罚性赔偿', "cheng'fa'xing'pei'chang", 3800, 'law', 'curated-law'),
    ('程颢', "cheng'hao", 1200, 'person', 'curated-person'),
    ('程开甲', "cheng'kai'jia", 1600, 'person', 'curated-person'),
    ('程砚秋', "cheng'yan'qiu", 1950, 'person', 'curated-person'),
    ('程颐', "cheng'yi", 1350, 'person', 'curated-person'),
    ('驰名商标', "chi'ming'shang'biao", 4100, 'law', 'curated-law'),
    ('持续部署', "chi'xu'bu'shu", 3900, 'it', 'curated-it'),
    ('持续交付', "chi'xu'jiao'fu", 4100, 'it', 'curated-it'),
    ('吃早点', "chi'zao'dian", 3100, 'daily', 'curated-daily'),
    ('迟子建', "chi'zi'jian", 2500, 'person', 'curated-person'),
    ('冲咖啡', "chong'ka'fei", 2900, 'daily', 'curated-daily'),
    ('稠密检索', "chou'mi'jian'suo", 2800, 'it', 'curated-it'),
    ('抽象语法树', "chou'xiang'yu'fa'shu", 3700, 'it', 'curated-it'),
    ('除斥期间', "chu'chi'qi'jian", 3500, 'law', 'curated-law'),
    ('除权除息', "chu'quan'chu'xi", 3700, 'finance', 'curated-finance'),
    ('褚遂良', "chu'sui'liang", 2600, 'person', 'curated-person'),
    ('出血性脑卒中', "chu'xue'xing'nao'cu'zhong", 3600, 'medical', 'curated-medical'),
    ('串行化隔离', "chuan'xing'hua'ge'li", 3300, 'it', 'curated-it'),
    ('吹空调', "chui'kong'tiao", 3800, 'daily', 'curated-daily'),
    ('垂直拆分', "chui'zhi'chai'fen", 3300, 'it', 'curated-it'),
    ('磁共振成像', "ci'gong'zhen'cheng'xiang", 4200, 'medical', 'curated-medical'),
    ('刺破公司面纱', "ci'po'gong'si'mian'sha", 3500, 'law', 'curated-law'),
    ('丛集性头痛', "cong'ji'xing'tou'tong", 2900, 'medical', 'curated-medical'),
    ('凑满减', "cou'man'jian", 3500, 'daily', 'curated-daily'),
    ('凑热闹', "cou're'nao", 3500, 'daily', 'curated-daily'),
    ('存款准备金率', "cun'kuan'zhun'bei'jin'lv", 4200, 'finance', 'curated-finance'),
    ('存算一体', "cun'suan'yi'ti", 3800, 'it', 'curated-it'),
    ('打包带走', "da'bao'dai'zou", 3500, 'daily', 'curated-daily'),
    ('打哈欠', "da'ha'qian", 2800, 'daily', 'curated-daily'),
    ('打卡上班', "da'ka'shang'ban", 3500, 'daily', 'curated-daily'),
    ('打卡下班', "da'ka'xia'ban", 3400, 'daily', 'curated-daily'),
    ('打瞌睡', "da'ke'shui", 3200, 'daily', 'curated-daily'),
    ('大排档', "da'pai'dang", 3300, 'daily', 'curated-daily'),
    ('大语言模型', "da'yu'yan'mo'xing", 4400, 'it', 'curated-it'),
    ('贷款市场报价利率', "dai'kuan'shi'chang'bao'jia'li'lv", 3900, 'finance', 'curated-finance'),
    ('代码解释器', "dai'ma'jie'shi'qi", 3700, 'it', 'curated-it'),
    ('戴望舒', "dai'wang'shu", 2100, 'person', 'curated-person'),
    ('带状疱疹', "dai'zhuang'pao'zhen", 4200, 'medical', 'curated-medical'),
    ('但丁', "dan'ding", 3100, 'person', 'curated-person'),
    ('氮化镓', "dan'hua'jia", 4100, 'it', 'curated-it'),
    ('道尔顿', "dao'er'dun", 2400, 'person', 'curated-person'),
    ('倒苦水', "dao'ku'shui", 2800, 'daily', 'curated-daily'),
    ('倒排索引', "dao'pai'suo'yin", 3400, 'it', 'curated-it'),
    ('德布罗意', "de'bu'luo'yi", 2300, 'person', 'curated-person'),
    ('德里达', "de'li'da", 2200, 'person', 'curated-person'),
    ('登革热', "deng'ge're", 3800, 'medical', 'curated-medical'),
    ('等离子刻蚀机', "deng'li'zi'ke'shi'ji", 3300, 'it', 'curated-it'),
    ('地方政府专项债', "di'fang'zheng'fu'zhuan'xiang'zhai", 4200, 'finance', 'curated-finance'),
    ('第谷', "di'gu", 1800, 'person', 'curated-person'),
    ('笛卡尔', "di'ka'er", 3100, 'person', 'curated-person'),
    ('狄拉克', "di'la'ke", 2400, 'person', 'curated-person'),
    ('地理标志', "di'li'biao'zhi", 3900, 'law', 'curated-law'),
    ('狄青', "di'qing", 1200, 'person', 'curated-person'),
    ('抵押权', "di'ya'quan", 3900, 'law', 'curated-law'),
    ('地役权', "di'yi'quan", 3600, 'law', 'curated-law'),
    ('抵用券', "di'yong'quan", 3400, 'daily', 'curated-daily'),
    ('缔约过失责任', "di'yue'guo'shi'ze'ren", 3700, 'law', 'curated-law'),
    ('低秩适应', "di'zhi'shi'ying", 3600, 'it', 'curated-it'),
    ('丁玲', "ding'ling", 2200, 'person', 'curated-person'),
    ('定向增发', "ding'xiang'zeng'fa", 3800, 'finance', 'curated-finance'),
    ('东方朔', "dong'fang'shuo", 2100, 'person', 'curated-person'),
    ('动量法', "dong'liang'fa", 3100, 'it', 'curated-it'),
    ('董希文', "dong'xi'wen", 2300, 'person', 'curated-person'),
    ('董仲舒', "dong'zhong'shu", 2800, 'person', 'curated-person'),
    ('兜风', "dou'feng", 2900, 'daily', 'curated-daily'),
    ('杜邦分析体系', "du'bang'fen'xi'ti'xi", 3400, 'finance', 'curated-finance'),
    ('独孤信', "du'gu'xin", 1600, 'person', 'curated-person'),
    ('杜牧', "du'mu", 2800, 'person', 'curated-person'),
    ('杜如晦', "du'ru'hui", 2400, 'person', 'curated-person'),
    ('读未提交', "du'wei'ti'jiao", 3200, 'it', 'curated-it'),
    ('读已提交', "du'yi'ti'jiao", 3400, 'it', 'curated-it'),
    ('独占指针', "du'zhan'zhi'zhen", 3300, 'it', 'curated-it'),
    ('端到端训练', "duan'dao'duan'xun'lian", 3800, 'it', 'curated-it'),
    ('短期融资券', "duan'qi'rong'zi'quan", 3800, 'finance', 'curated-finance'),
    ('短暂性脑缺血发作', "duan'zan'xing'nao'que'xue'fa'zuo", 3400, 'medical', 'curated-medical'),
    ('对比学习', "dui'bi'xue'xi", 3600, 'it', 'curated-it'),
    ('对赌协议', "dui'du'xie'yi", 3900, 'law', 'curated-law'),
    ('对象存储', "dui'xiang'chu'cun", 3900, 'it', 'curated-it'),
    ('多版本并发控制', "duo'ban'ben'bing'fa'kong'zhi", 3800, 'it', 'curated-it'),
    ('多多包涵', "duo'duo'bao'han", 2900, 'daily', 'curated-daily'),
    ('多发性硬化', "duo'fa'xing'ying'hua", 3400, 'medical', 'curated-medical'),
    ('多模态理解', "duo'mo'tai'li'jie", 3900, 'it', 'curated-it'),
    ('多潘立酮', "duo'pan'li'tong", 3900, 'medical', 'curated-medical'),
    ('多头注意力', "duo'tou'zhu'yi'li", 3800, 'it', 'curated-it'),
    ('鄂维南', "e'wei'nan", 2600, 'person', 'curated-person'),
    ('二级资本债', "er'ji'zi'ben'zhai", 3600, 'finance', 'curated-finance'),
    ('二尖瓣关闭不全', "er'jian'ban'guan'bi'bu'quan", 3400, 'medical', 'curated-medical'),
    ('二尖瓣狭窄', "er'jian'ban'xia'zhai", 3400, 'medical', 'curated-medical'),
    ('法拉第', "fa'la'di", 2900, 'person', 'curated-person'),
    ('发牢骚', "fa'lao'sao", 3100, 'daily', 'curated-daily'),
    ('发明专利', "fa'ming'zhuan'li", 4300, 'law', 'curated-law'),
    ('发票抬头', "fa'piao'tai'tou", 3600, 'daily', 'curated-daily'),
    ('犯不着', "fan'bu'zhao", 3200, 'daily', 'curated-daily'),
    ('反不正当竞争法', "fan'bu'zheng'dang'jing'zheng'fa", 4300, 'law', 'curated-law'),
    ('范雎', "fan'ju", 880, 'person', 'curated-person'),
    ('反流性食管炎', "fan'liu'xing'shi'guan'yan", 3700, 'medical', 'curated-medical'),
    ('范文正公', "fan'wen'zheng'gong", 280, 'person', 'curated-person'),
    ('反向传播', "fan'xiang'chuan'bo", 4300, 'it', 'curated-it'),
    ('范晔', "fan'ye", 1300, 'person', 'curated-person'),
    ('房室传导阻滞', "fang'shi'chuan'dao'zu'zhi", 3500, 'medical', 'curated-medical'),
    ('防卫过当', "fang'wei'guo'dang", 3800, 'law', 'curated-law'),
    ('房玄龄', "fang'xuan'ling", 2900, 'person', 'curated-person'),
    ('菲茨杰拉德', "fei'ci'jie'la'de", 2800, 'person', 'curated-person'),
    ('肺动脉栓塞', "fei'dong'mai'shuan'sai", 3600, 'medical', 'curated-medical'),
    ('费尔巴哈', "fei'er'ba'ha", 2100, 'person', 'curated-person'),
    ('肥厚型心肌病', "fei'hou'xing'xin'ji'bing", 3400, 'medical', 'curated-medical'),
    ('费马', "fei'ma", 2700, 'person', 'curated-person'),
    ('废名', "fei'ming", 1150, 'person', 'curated-person'),
    ('分布式存储', "fen'bu'shi'chu'cun", 4200, 'it', 'curated-it'),
    ('分布式事务', "fen'bu'shi'shi'wu", 4200, 'it', 'curated-it'),
    ('分布式文件系统', "fen'bu'shi'wen'jian'xi'tong", 4100, 'it', 'curated-it'),
    ('分词器', "fen'ci'qi", 3700, 'it', 'curated-it'),
    ('分治算法', "fen'zhi'suan'fa", 3600, 'it', 'curated-it'),
    ('冯端', "feng'duan", 1100, 'person', 'curated-person'),
    ('冯康', "feng'kang", 2100, 'person', 'curated-person'),
    ('冯梦龙', "feng'meng'long", 680, 'person', 'curated-person'),
    ('风湿性心脏病', "feng'shi'xing'xin'zang'bing", 3700, 'medical', 'curated-medical'),
    ('冯友兰', "feng'you'lan", 3100, 'person', 'curated-person'),
    ('冯至', "feng'zhi", 1300, 'person', 'curated-person'),
    ('丰子恺', "feng'zi'kai", 3700, 'person', 'curated-person'),
    ('傅抱石', "fu'bao'shi", 2900, 'person', 'curated-person'),
    ('伏尔泰', "fu'er'tai", 3000, 'person', 'curated-person'),
    ('附件下载', "fu'jian'xia'zai", 3700, 'daily', 'curated-daily'),
    ('福柯', "fu'ke", 2600, 'person', 'curated-person'),
    ('福克纳', "fu'ke'na", 2600, 'person', 'curated-person'),
    ('弗莱明', "fu'lai'ming", 2400, 'person', 'curated-person'),
    ('弗里德曼', "fu'li'de'man", 2600, 'person', 'curated-person'),
    ('傅里叶', "fu'li'ye", 2900, 'person', 'curated-person'),
    ('福楼拜', "fu'lou'bai", 2500, 'person', 'curated-person'),
    ('复盘会', "fu'pan'hui", 3800, 'daily', 'curated-daily'),
    ('傅斯年', "fu'si'nian", 2800, 'person', 'curated-person'),
    ('附条件合同', "fu'tiao'jian'he'tong", 3400, 'law', 'curated-law'),
    ('改简历', "gai'jian'li", 3500, 'daily', 'curated-daily'),
    ('盖叫天', "gai'jiao'tian", 1600, 'person', 'curated-person'),
    ('改善伙食', "gai'shan'huo'shi", 3300, 'daily', 'curated-daily'),
    ('改天再约', "gai'tian'zai'yue", 3100, 'daily', 'curated-daily'),
    ('赶大集', "gan'da'ji", 2600, 'daily', 'curated-daily'),
    ('感染性心内膜炎', "gan'ran'xing'xin'nei'mo'yan", 3300, 'medical', 'curated-medical'),
    ('感染性休克', "gan'ran'xing'xiu'ke", 3700, 'medical', 'curated-medical'),
    ('肝性脑病', "gan'xing'nao'bing", 3400, 'medical', 'curated-medical'),
    ('甘英', "gan'ying", 650, 'person', 'curated-person'),
    ('干燥综合征', "gan'zao'zong'he'zheng", 3500, 'medical', 'curated-medical'),
    ('高带宽内存', "gao'dai'kuan'nei'cun", 3900, 'it', 'curated-it'),
    ('高度盖然性', "gao'du'gai'ran'xing", 3600, 'law', 'curated-law'),
    ('高适', "gao'shi", 1900, 'person', 'curated-person'),
    ('高斯', "gao'si", 3400, 'person', 'curated-person'),
    ('高通量测序', "gao'tong'liang'ce'xu", 3800, 'medical', 'curated-medical'),
    ('高血压危象', "gao'xue'ya'wei'xiang", 3400, 'medical', 'curated-medical'),
    ('哥白尼', "ge'bai'ni", 3100, 'person', 'curated-person'),
    ('歌德', "ge'de", 3200, 'person', 'curated-person'),
    ('哥德尔', "ge'de'er", 2400, 'person', 'curated-person'),
    ('格非', "ge'fei", 2100, 'person', 'curated-person'),
    ('格式条款', "ge'shi'tiao'kuan", 3800, 'law', 'curated-law'),
    ('葛庭燧', "ge'ting'sui", 910, 'person', 'curated-person'),
    ('根管治疗', "gen'guan'zhi'liao", 4200, 'medical', 'curated-medical'),
    ('根因分析', "gen'yin'fen'xi", 3600, 'daily', 'curated-daily'),
    ('公积金', "gong'ji'jin", 4500, 'daily', 'curated-daily'),
    ('宫颈癌疫苗', "gong'jing'ai'yi'miao", 4400, 'medical', 'curated-medical'),
    ('公开市场操作', "gong'kai'shi'chang'cao'zuo", 3800, 'finance', 'curated-finance'),
    ('公募基金', "gong'mu'ji'jin", 4500, 'finance', 'curated-finance'),
    ('共识算法', "gong'shi'suan'fa", 3800, 'it', 'curated-it'),
    ('公孙策', "gong'sun'ce", 1900, 'person', 'curated-person'),
    ('共同犯罪', "gong'tong'fan'zui", 3900, 'law', 'curated-law'),
    ('共享屏幕', "gong'xiang'ping'mu", 3900, 'daily', 'curated-daily'),
    ('共享指针', "gong'xiang'zhi'zhen", 3400, 'it', 'curated-it'),
    ('供应链金融', "gong'ying'lian'jin'rong", 4100, 'finance', 'curated-finance'),
    ('龚自珍', "gong'zi'zhen", 2400, 'person', 'curated-person'),
    ('工作交接', "gong'zuo'jiao'jie", 3600, 'daily', 'curated-daily'),
    ('购物车', "gou'wu'che", 4100, 'daily', 'curated-daily'),
    ('谷超豪', "gu'chao'hao", 2100, 'person', 'curated-person'),
    ('顾城', "gu'cheng", 2800, 'person', 'curated-person'),
    ('顾方舟', "gu'fang'zhou", 2400, 'person', 'curated-person'),
    ('股骨头坏死', "gu'gu'tou'huai'si", 3700, 'medical', 'curated-medical'),
    ('顾颉刚', "gu'jie'gang", 2600, 'person', 'curated-person'),
    ('股权激励', "gu'quan'ji'li", 4200, 'finance', 'curated-finance'),
    ('股权质押', "gu'quan'zhi'ya", 4200, 'law', 'curated-law'),
    ('顾诵芬', "gu'song'fen", 1700, 'person', 'curated-person'),
    ('股息率', "gu'xi'lv", 4100, 'finance', 'curated-finance'),
    ('姑息治疗', "gu'xi'zhi'liao", 3600, 'medical', 'curated-medical'),
    ('顾炎武', "gu'yan'wu", 2100, 'person', 'curated-person'),
    ('股指期货', "gu'zhi'qi'huo", 4100, 'finance', 'curated-finance'),
    ('骨质疏松', "gu'zhi'shu'song", 4300, 'medical', 'curated-medical'),
    ('关东煮', "guan'dong'zhu", 3700, 'daily', 'curated-daily'),
    ('关汉卿', "guan'han'qing", 1200, 'person', 'curated-person'),
    ('关山月', "guan'shan'yue", 1750, 'person', 'curated-person'),
    ('灌汤包', "guan'tang'bao", 3700, 'daily', 'curated-daily'),
    ('关天培', "guan'tian'pei", 1100, 'person', 'curated-person'),
    ('关系抽取', "guan'xi'chou'qu", 3300, 'it', 'curated-it'),
    ('关云长', "guan'yun'chang", 1950, 'person', 'curated-person'),
    ('贯云石', "guan'yun'shi", 1300, 'person', 'curated-person'),
    ('冠状动脉硬化', "guan'zhuang'dong'mai'ying'hua", 3800, 'medical', 'curated-medical'),
    ('逛超市', "guang'chao'shi", 3900, 'daily', 'curated-daily'),
    ('广度优先搜索', "guang'du'you'xian'sou'suo", 3900, 'it', 'curated-it'),
    ('光刻机', "guang'ke'ji", 4600, 'it', 'curated-it'),
    ('逛商场', "guang'shang'chang", 3700, 'daily', 'curated-daily'),
    ('逛夜市', "guang'ye'shi", 3400, 'daily', 'curated-daily'),
    ('鬼谷子', "gui'gu'zi", 2600, 'person', 'curated-person'),
    ('硅光芯片', "gui'guang'xin'pian", 3600, 'it', 'curated-it'),
    ('硅通孔', "gui'tong'kong", 3400, 'it', 'curated-it'),
    ('果不其然', "guo'bu'qi'ran", 3700, 'daily', 'curated-daily'),
    ('郭奉孝', "guo'feng'xiao", 1150, 'person', 'curated-person'),
    ('果戈理', "guo'ge'li", 2600, 'person', 'curated-person'),
    ('国考', "guo'kao", 4100, 'daily', 'curated-daily'),
    ('过敏性鼻炎', "guo'min'xing'bi'yan", 4400, 'medical', 'curated-medical'),
    ('郭守敬', "guo'shou'jing", 1850, 'person', 'curated-person'),
    ('郭永怀', "guo'yong'huai", 1800, 'person', 'curated-person'),
    ('国债逆回购', "guo'zhai'ni'hui'gou", 3900, 'finance', 'curated-finance'),
    ('国债期货', "guo'zhai'qi'huo", 3800, 'finance', 'curated-finance'),
    ('哈勃', "ha'bo", 2400, 'person', 'curated-person'),
    ('哈耶克', "ha'ye'ke", 2800, 'person', 'curated-person'),
    ('海森堡', "hai'sen'bao", 2700, 'person', 'curated-person'),
    ('海子', "hai'zi", 3400, 'person', 'curated-person'),
    ('韩世忠', "han'shi'zhong", 1800, 'person', 'curated-person'),
    ('韩愈', "han'yu", 3200, 'person', 'curated-person'),
    ('好说好说', "hao'shuo'hao'shuo", 2600, 'daily', 'curated-daily'),
    ('薅羊毛', "hao'yang'mao", 3700, 'daily', 'curated-daily'),
    ('喝咖啡', "he'ka'fei", 4200, 'daily', 'curated-daily'),
    ('贺绿汀', "he'lv'ting", 1800, 'person', 'curated-person'),
    ('荷马', "he'ma", 2900, 'person', 'curated-person'),
    ('何其芳', "he'qi'fang", 1450, 'person', 'curated-person'),
    ('喝下午茶', "he'xia'wu'cha", 3300, 'daily', 'curated-daily'),
    ('何泽慧', "he'ze'hui", 2600, 'person', 'curated-person'),
    ('贺知章', "he'zhi'zhang", 2100, 'person', 'curated-person'),
    ('贺铸', "he'zhu", 1950, 'person', 'curated-person'),
    ('很难讲', "hen'nan'jiang", 2800, 'daily', 'curated-daily'),
    ('划算', "hua'suan", 4100, 'daily', 'curated-daily'),
    ('化学发光免疫分析', "hua'xue'fa'guang'mian'yi'fen'xi", 3500, 'medical', 'curated-medical'),
    ('化学机械抛光', "hua'xue'ji'xie'pao'guang", 3400, 'it', 'curated-it'),
    ('华彦钧', "hua'yan'jun", 1500, 'person', 'curated-person'),
    ('坏账准备', "huai'zhang'zhun'bei", 3600, 'finance', 'curated-finance'),
    ('缓冲区溢出', "huan'chong'qu'yi'chu", 3900, 'it', 'curated-it'),
    ('缓存穿透', "huan'cun'chuan'tou", 3900, 'it', 'curated-it'),
    ('缓存击穿', "huan'cun'ji'chuan", 3900, 'it', 'curated-it'),
    ('缓存雪崩', "huan'cun'xue'beng", 3900, 'it', 'curated-it'),
    ('幻觉抑制', "huan'jue'yi'zhi", 3300, 'it', 'curated-it'),
    ('环形缓冲区', "huan'xing'huan'chong'qu", 3500, 'it', 'curated-it'),
    ('黄斑变性', "huang'ban'bian'xing", 3600, 'medical', 'curated-medical'),
    ('黄宾虹', "huang'bin'hong", 1900, 'person', 'curated-person'),
    ('皇甫谧', "huang'fu'mi", 920, 'person', 'curated-person'),
    ('黄家驷', "huang'jia'si", 1150, 'person', 'curated-person'),
    ('黄昆', "huang'kun", 1600, 'person', 'curated-person'),
    ('恍然大悟', "huang'ran'da'wu", 3900, 'daily', 'curated-daily'),
    ('黄庭坚', "huang'ting'jian", 3600, 'person', 'curated-person'),
    ('黄纬禄', "huang'wei'lu", 1300, 'person', 'curated-person'),
    ('黄旭华', "huang'xu'hua", 2600, 'person', 'curated-person'),
    ('黄宗羲', "huang'zong'xi", 1950, 'person', 'curated-person'),
    ('汇报工作', "hui'bao'gong'zuo", 3900, 'daily', 'curated-daily'),
    ('回复邮件', "hui'fu'you'jian", 3600, 'daily', 'curated-daily'),
    ('混合精度训练', "hun'he'jing'du'xun'lian", 3600, 'it', 'curated-it'),
    ('霍布斯', "huo'bu'si", 2300, 'person', 'curated-person'),
    ('霍去病', "huo'qu'bing", 4100, 'person', 'curated-person'),
    ('集成测试', "ji'cheng'ce'shi", 4100, 'it', 'curated-it'),
    ('基础模型', "ji'chu'mo'xing", 3800, 'it', 'curated-it'),
    ('济慈', "ji'ci", 2400, 'person', 'curated-person'),
    ('挤地铁', "ji'di'tie", 3800, 'daily', 'curated-daily'),
    ('季度考核', "ji'du'kao'he", 3400, 'daily', 'curated-daily'),
    ('激活重算', "ji'huo'chong'suan", 3200, 'it', 'curated-it'),
    ('激活函数', "ji'huo'han'shu", 4200, 'it', 'curated-it'),
    ('嵇康', "ji'kang", 1600, 'person', 'curated-person'),
    ('寄快递', "ji'kuai'di", 4200, 'daily', 'curated-daily'),
    ('计算机断层扫描', "ji'suan'ji'duan'ceng'sao'miao", 3900, 'medical', 'curated-medical'),
    ('脊髓灰质炎', "ji'sui'hui'zhi'yan", 3600, 'medical', 'curated-medical'),
    ('季羡林', "ji'xian'lin", 3900, 'person', 'curated-person'),
    ('绩效奖金', "ji'xiao'jiang'jin", 3800, 'daily', 'curated-daily'),
    ('绩效考核', "ji'xiao'kao'he", 4200, 'daily', 'curated-daily'),
    ('急性冠状动脉综合征', "ji'xing'guan'zhuang'dong'mai'zong'he'zheng", 3700, 'medical', 'curated-medical'),
    ('急性呼吸窘迫综合征', "ji'xing'hu'xi'jiong'po'zong'he'zheng", 3500, 'medical', 'curated-medical'),
    ('急性肾损伤', "ji'xing'shen'sun'shang", 3600, 'medical', 'curated-medical'),
    ('急性左心衰竭', "ji'xing'zuo'xin'shuai'jie", 3400, 'medical', 'curated-medical'),
    ('极紫外光刻', "ji'zi'wai'guang'ke", 3900, 'it', 'curated-it'),
    ('加缪', "jia'miu", 2700, 'person', 'curated-person'),
    ('贾平凹', "jia'ping'wa", 3300, 'person', 'curated-person'),
    ('贾思勰', "jia'si'xie", 1600, 'person', 'curated-person'),
    ('贾文和', "jia'wen'he", 890, 'person', 'curated-person'),
    ('贾谊', "jia'yi", 2200, 'person', 'curated-person'),
    ('甲状腺功能减退', "jia'zhuang'xian'gong'neng'jian'tui", 3900, 'medical', 'curated-medical'),
    ('甲状腺结节', "jia'zhuang'xian'jie'jie", 4300, 'medical', 'curated-medical'),
    ('煎饼果子', "jian'bing'guo'zi", 3900, 'daily', 'curated-daily'),
    ('捡漏', "jian'lou", 3300, 'daily', 'curated-daily'),
    ('检索增强', "jian'suo'zeng'qiang", 3800, 'it', 'curated-it'),
    ('检索增强生成', "jian'suo'zeng'qiang'sheng'cheng", 3700, 'it', 'curated-it'),
    ('建筑物区分所有权', "jian'zhu'wu'qu'fen'suo'you'quan", 3600, 'law', 'curated-law'),
    ('姜伯驹', "jiang'bo'ju", 1700, 'person', 'curated-person'),
    ('姜伯约', "jiang'bo'yue", 920, 'person', 'curated-person'),
    ('讲道理', "jiang'dao'li", 3900, 'daily', 'curated-daily'),
    ('姜立夫', "jiang'li'fu", 980, 'person', 'curated-person'),
    ('奖励模型', "jiang'li'mo'xing", 3700, 'it', 'curated-it'),
    ('蒋兆和', "jiang'zhao'he", 2100, 'person', 'curated-person'),
    ('交叉熵损失', "jiao'cha'shang'sun'shi", 3500, 'it', 'curated-it'),
    ('交叉注意力', "jiao'cha'zhu'yi'li", 3400, 'it', 'curated-it'),
    ('教师编', "jiao'shi'bian", 3800, 'daily', 'curated-daily'),
    ('结肠镜', "jie'chang'jing", 4100, 'medical', 'curated-medical'),
    ('结构化输出', "jie'gou'hua'shu'chu", 3600, 'it', 'curated-it'),
    ('借过一下', "jie'guo'yi'xia", 3100, 'daily', 'curated-daily'),
    ('借你吉言', "jie'ni'ji'yan", 2800, 'daily', 'curated-daily'),
    ('结题报告', "jie'ti'bao'gao", 3500, 'daily', 'curated-daily'),
    ('尽管放心', "jin'guan'fang'xin", 2900, 'daily', 'curated-daily'),
    ('紧急避险', "jin'ji'bi'xian", 4100, 'law', 'curated-law'),
    ('近邻搜索', "jin'lin'sou'suo", 3100, 'it', 'curated-it'),
    ('靳尚谊', "jin'shang'yi", 1700, 'person', 'curated-person'),
    ('晋升答辩', "jin'sheng'da'bian", 3400, 'daily', 'curated-daily'),
    ('金岳霖', "jin'yue'lin", 2700, 'person', 'curated-person'),
    ('静态链接库', "jing'tai'lian'jie'ku", 3700, 'it', 'curated-it'),
    ('竞态条件', "jing'tai'tiao'jian", 3800, 'it', 'curated-it'),
    ('竞业限制', "jing'ye'xian'zhi", 3300, 'daily', 'curated-daily'),
    ('竞业协议', "jing'ye'xie'yi", 3500, 'daily', 'curated-daily'),
    ('净资产收益率', "jing'zi'chan'shou'yi'lv", 4200, 'finance', 'curated-finance'),
    ('纠删码', "jiu'shan'ma", 3300, 'it', 'curated-it'),
    ('聚簇索引', "ju'cu'suo'yin", 3600, 'it', 'curated-it'),
    ('举手之劳', "ju'shou'zhi'lao", 2900, 'daily', 'curated-daily'),
    ('举证倒置', "ju'zheng'dao'zhi", 3500, 'law', 'curated-law'),
    ('卷积神经网络', "juan'ji'shen'jing'wang'luo", 4500, 'it', 'curated-it'),
    ('均方误差', "jun'fang'wu'cha", 3300, 'it', 'curated-it'),
    ('卡布奇诺', "ka'bu'qi'nuo", 3800, 'daily', 'curated-daily'),
    ('卡尔曼滤波', "ka'er'man'lv'bo", 3900, 'finance', 'curated-finance'),
    ('卡夫卡', "ka'fu'ka", 3500, 'person', 'curated-person'),
    ('卡介苗', "ka'jie'miao", 3900, 'medical', 'curated-medical'),
    ('凯恩斯', "kai'en'si", 3100, 'person', 'curated-person'),
    ('开普勒', "kai'pu'le", 2700, 'person', 'curated-person'),
    ('开题报告', "kai'ti'bao'gao", 4100, 'daily', 'curated-daily'),
    ('开玩笑', "kai'wan'xiao", 3600, 'daily', 'curated-daily'),
    ('侃大山', "kan'da'shan", 2700, 'daily', 'curated-daily'),
    ('砍价', "kan'jia", 3200, 'daily', 'curated-daily'),
    ('看情况', "kan'qing'kuang", 4200, 'daily', 'curated-daily'),
    ('抗组胺药', "kang'zu'an'yao", 3800, 'medical', 'curated-medical'),
    ('考公党', "kao'gong'dang", 3700, 'daily', 'curated-daily'),
    ('犒劳自己', "kao'lao'zi'ji", 3400, 'daily', 'curated-daily'),
    ('烤冷面', "kao'leng'mian", 3600, 'daily', 'curated-daily'),
    ('考勤记录', "kao'qin'ji'lu", 3200, 'daily', 'curated-daily'),
    ('考研党', "kao'yan'dang", 3800, 'daily', 'curated-daily'),
    ('可不是嘛', "ke'bu'shi'ma", 2800, 'daily', 'curated-daily'),
    ('可重复读', "ke'chong'fu'du", 3600, 'it', 'curated-it'),
    ('科创板', "ke'chuang'ban", 4400, 'finance', 'curated-finance'),
    ('柯尔莫哥洛夫', "ke'er'mo'ge'luo'fu", 1850, 'person', 'curated-person'),
    ('科赫', "ke'he", 2100, 'person', 'curated-person'),
    ('克里克', "ke'li'ke", 2300, 'person', 'curated-person'),
    ('克罗恩病', "ke'luo'en'bing", 3500, 'medical', 'curated-medical'),
    ('柯西', "ke'xi", 2400, 'person', 'curated-person'),
    ('可转换公司债券', "ke'zhuan'huan'gong'si'zhai'quan", 3900, 'finance', 'curated-finance'),
    ('控制流图', "kong'zhi'liu'tu", 3200, 'it', 'curated-it'),
    ('寇准', "kou'zhun", 1300, 'person', 'curated-person'),
    ('哭笑不得', "ku'xiao'bu'de", 3900, 'daily', 'curated-daily'),
    ('跨式期权', "kua'shi'qi'quan", 3600, 'finance', 'curated-finance'),
    ('宽禁带半导体', "kuan'jin'dai'ban'dao'ti", 3700, 'it', 'curated-it'),
    ('狂犬病', "kuang'quan'bing", 4100, 'medical', 'curated-medical'),
    ('狂犬病疫苗', "kuang'quan'bing'yi'miao", 4300, 'medical', 'curated-medical'),
    ('溃疡性结肠炎', "kui'yang'xing'jie'chang'yan", 3700, 'medical', 'curated-medical'),
    ('扩散模型', "kuo'san'mo'xing", 3800, 'it', 'curated-it'),
    ('扩张型心肌病', "kuo'zhang'xing'xin'ji'bing", 3500, 'medical', 'curated-medical'),
    ('拉格朗日', "la'ge'lang'ri", 2600, 'person', 'curated-person'),
    ('拉普拉斯', "la'pu'la'si", 2500, 'person', 'curated-person'),
    ('拉瓦锡', "la'wa'xi", 2500, 'person', 'curated-person'),
    ('莱布尼茨', "lai'bu'ni'ci", 2600, 'person', 'curated-person'),
    ('赖床', "lai'chuang", 3100, 'daily', 'curated-daily'),
    ('来日方长', "lai'ri'fang'chang", 3600, 'daily', 'curated-daily'),
    ('兰索拉唑', "lan'suo'la'zuo", 3900, 'medical', 'curated-medical'),
    ('酪氨酸激酶抑制剂', "lao'an'suan'ji'mei'yi'zhi'ji", 3600, 'medical', 'curated-medical'),
    ('劳动合同', "lao'dong'he'tong", 4100, 'daily', 'curated-daily'),
    ('劳动合同法', "lao'dong'he'tong'fa", 4500, 'law', 'curated-law'),
    ('劳驾了', "lao'jia'liao", 2500, 'daily', 'curated-daily'),
    ('唠嗑', "lao'ke", 3400, 'daily', 'curated-daily'),
    ('乐观并发控制', "le'guan'bing'fa'kong'zhi", 3500, 'it', 'curated-it'),
    ('雷贝拉唑', "lei'bei'la'zuo", 4100, 'medical', 'curated-medical'),
    ('类风湿关节炎', "lei'feng'shi'guan'jie'yan", 4100, 'medical', 'curated-medical'),
    ('累计摊销', "lei'ji'tan'xiao", 3500, 'finance', 'curated-finance'),
    ('累计折旧', "lei'ji'zhe'jiu", 3700, 'finance', 'curated-finance'),
    ('冷萃咖啡', "leng'cui'ka'fei", 3600, 'daily', 'curated-daily'),
    ('离岸人民币', "li'an'ren'min'bi", 3900, 'finance', 'curated-finance'),
    ('里程碑', "li'cheng'bei", 4100, 'daily', 'curated-daily'),
    ('郦道元', "li'dao'yuan", 2100, 'person', 'curated-person'),
    ('李飞飞', "li'fei'fei", 3700, 'person', 'curated-person'),
    ('离婚冷静期', "li'hun'leng'jing'qi", 3900, 'law', 'curated-law'),
    ('李嘉图', "li'jia'tu", 2200, 'person', 'curated-person'),
    ('李可染', "li'ke'ran", 2400, 'person', 'curated-person'),
    ('黎曼', "li'man", 2800, 'person', 'curated-person'),
    ('李商隐', "li'shang'yin", 3500, 'person', 'curated-person'),
    ('李四光', "li'si'guang", 2800, 'person', 'curated-person'),
    ('黎雄才', "li'xiong'cai", 1500, 'person', 'curated-person'),
    ('李贽', "li'zhi", 1400, 'person', 'curated-person'),
    ('李之仪', "li'zhi'yi", 1650, 'person', 'curated-person'),
    ('离职证明', "li'zhi'zheng'ming", 3700, 'daily', 'curated-daily'),
    ('联调测试', "lian'tiao'ce'shi", 3800, 'daily', 'curated-daily'),
    ('连轴转', "lian'zhou'zhuan", 3300, 'daily', 'curated-daily'),
    ('量化交易', "liang'hua'jiao'yi", 4200, 'finance', 'curated-finance'),
    ('两阶段提交', "liang'jie'duan'ti'jiao", 3700, 'it', 'curated-it'),
    ('梁守槃', "liang'shou'pan", 1100, 'person', 'curated-person'),
    ('梁思成', "liang'si'cheng", 3800, 'person', 'curated-person'),
    ('梁思礼', "liang'si'li", 1400, 'person', 'curated-person'),
    ('量子比特', "liang'zi'bi'te", 3800, 'it', 'curated-it'),
    ('量子叠加', "liang'zi'die'jia", 3600, 'it', 'curated-it'),
    ('量子纠缠', "liang'zi'jiu'chan", 4300, 'it', 'curated-it'),
    ('林风眠', "lin'feng'mian", 2100, 'person', 'curated-person'),
    ('林徽因', "lin'hui'yin", 4100, 'person', 'curated-person'),
    ('临键锁', "lin'jian'suo", 3100, 'it', 'curated-it'),
    ('林巧稚', "lin'qiao'zhi", 2100, 'person', 'curated-person'),
    ('令狐冲', "ling'hu'chong", 3800, 'person', 'curated-person'),
    ('令狐楚', "ling'hu'chu", 1200, 'person', 'curated-person'),
    ('零花钱', "ling'hua'qian", 3100, 'daily', 'curated-daily'),
    ('零拷贝技术', "ling'kao'bei'ji'shu", 3600, 'it', 'curated-it'),
    ('凌濛初', "ling'meng'chu", 450, 'person', 'curated-person'),
    ('刘海粟', "liu'hai'su", 1850, 'person', 'curated-person'),
    ('刘伶', "liu'ling", 720, 'person', 'curated-person'),
    ('柳青', "liu'qing", 1600, 'person', 'curated-person'),
    ('流式细胞术', "liu'shi'xi'bao'shu", 3700, 'medical', 'curated-medical'),
    ('流水线并行', "liu'shui'xian'bing'xing", 3600, 'it', 'curated-it'),
    ('刘天华', "liu'tian'hua", 1750, 'person', 'curated-person'),
    ('遛弯儿', "liu'wan'er", 3100, 'daily', 'curated-daily'),
    ('流行性乙型脑炎', "liu'xing'xing'yi'xing'nao'yan", 3500, 'medical', 'curated-medical'),
    ('刘禹锡', "liu'yu'xi", 3200, 'person', 'curated-person'),
    ('刘震云', "liu'zhen'yun", 2700, 'person', 'curated-person'),
    ('留置权', "liu'zhi'quan", 3600, 'law', 'curated-law'),
    ('柳宗元', "liu'zong'yuan", 3100, 'person', 'curated-person'),
    ('路边摊', "lu'bian'tan", 3200, 'daily', 'curated-daily'),
    ('陆伯言', "lu'bo'yan", 860, 'person', 'curated-person'),
    ('陆九渊', "lu'jiu'yuan", 1500, 'person', 'curated-person'),
    ('陆抗', "lu'kang", 920, 'person', 'curated-person'),
    ('颅内动脉瘤', "lu'nei'dong'mai'liu", 3200, 'medical', 'curated-medical'),
    ('卢瑟福', "lu'se'fu", 2600, 'person', 'curated-person'),
    ('路上小心', "lu'shang'xiao'xin", 3200, 'daily', 'curated-daily'),
    ('陆士衡', "lu'shi'heng", 780, 'person', 'curated-person'),
    ('陆秀夫', "lu'xiu'fu", 1100, 'person', 'curated-person'),
    ('路遥', "lu'yao", 3800, 'person', 'curated-person'),
    ('录用通知书', "lu'yong'tong'zhi'shu", 3600, 'daily', 'curated-daily'),
    ('陆元九', "lu'yuan'jiu", 1950, 'person', 'curated-person'),
    ('卢照邻', "lu'zhao'lin", 960, 'person', 'curated-person'),
    ('鲁子敬', "lu'zi'jing", 820, 'person', 'curated-person'),
    ('伦琴', "lun'qin", 2300, 'person', 'curated-person'),
    ('骆宾王', "luo'bin'wang", 2300, 'person', 'curated-person'),
    ('罗工柳', "luo'gong'liu", 1250, 'person', 'curated-person'),
    ('逻辑时钟', "luo'ji'shi'zhong", 3200, 'it', 'curated-it'),
    ('洛克', "luo'ke", 2600, 'person', 'curated-person'),
    ('螺蛳粉', "luo'si'fen", 4200, 'daily', 'curated-daily'),
    ('洛索洛芬钠', "luo'suo'luo'fen'na", 3900, 'medical', 'curated-medical'),
    ('马尔克斯', "ma'er'ke'si", 3600, 'person', 'curated-person'),
    ('麻辣香锅', "ma'la'xiang'guo", 3900, 'daily', 'curated-daily'),
    ('马连良', "ma'lian'liang", 1900, 'person', 'curated-person'),
    ('马上来', "ma'shang'lai", 3300, 'daily', 'curated-daily'),
    ('马思聪', "ma'si'cong", 1650, 'person', 'curated-person'),
    ('马致远', "ma'zhi'yuan", 920, 'person', 'curated-person'),
    ('慢查询日志', "man'cha'xun'ri'zhi", 3600, 'it', 'curated-it'),
    ('满额包邮', "man'e'bao'you", 3600, 'daily', 'curated-daily'),
    ('慢慢来', "man'man'lai", 3500, 'daily', 'curated-daily'),
    ('慢性肾衰竭', "man'xing'shen'shuai'jie", 3900, 'medical', 'curated-medical'),
    ('慢性心力衰竭', "man'xing'xin'li'shuai'jie", 3800, 'medical', 'curated-medical'),
    ('慢性阻塞性肺疾病', "man'xing'zu'sai'xing'fei'ji'bing", 4200, 'medical', 'curated-medical'),
    ('茅以升', "mao'yi'sheng", 2500, 'person', 'curated-person'),
    ('每股收益', "mei'gu'shou'yi", 4400, 'finance', 'curated-finance'),
    ('没话说', "mei'hua'shuo", 3600, 'daily', 'curated-daily'),
    ('美尼尔氏综合征', "mei'ni'er'shi'zong'he'zheng", 3300, 'medical', 'curated-medical'),
    ('美式咖啡', "mei'shi'ka'fei", 4200, 'daily', 'curated-daily'),
    ('没准儿', "mei'zhun'er", 2200, 'daily', 'curated-daily'),
    ('门静脉高压', "men'jing'mai'gao'ya", 3500, 'medical', 'curated-medical'),
    ('门控循环单元', "men'kong'xun'huan'dan'yuan", 3300, 'it', 'curated-it'),
    ('孟德尔', "meng'de'er", 2700, 'person', 'curated-person'),
    ('孟德斯鸠', "meng'de'si'jiu", 2800, 'person', 'curated-person'),
    ('孟浩然', "meng'hao'ran", 2900, 'person', 'curated-person'),
    ('蒙脱石散', "meng'tuo'shi'san", 4300, 'medical', 'curated-medical'),
    ('米芾', "mi'fu", 3100, 'person', 'curated-person'),
    ('弥散性血管内凝血', "mi'san'xing'xue'guan'nei'ning'xue", 3600, 'medical', 'curated-medical'),
    ('面神经麻痹', "mian'shen'jing'ma'bi", 3400, 'medical', 'curated-medical'),
    ('面试通知', "mian'shi'tong'zhi", 3900, 'daily', 'curated-daily'),
    ('免疫球蛋白', "mian'yi'qiu'dan'bai", 4200, 'medical', 'curated-medical'),
    ('莫名其妙', "mo'ming'qi'miao", 4100, 'daily', 'curated-daily'),
    ('莫泊桑', "mo'po'sang", 2900, 'person', 'curated-person'),
    ('万俟咏', "mo'qi'yong", 1400, 'person', 'curated-person'),
    ('莫沙必利', "mo'sha'bi'li", 3800, 'medical', 'curated-medical'),
    ('模型剪枝', "mo'xing'jian'zhi", 3400, 'it', 'curated-it'),
    ('莫言', "mo'yan", 4500, 'person', 'curated-person'),
    ('目标检测', "mu'biao'jian'ce", 4100, 'it', 'curated-it'),
    ('穆旦', "mu'dan", 1650, 'person', 'curated-person'),
    ('慕容冲', "mu'rong'chong", 1500, 'person', 'curated-person'),
    ('拿快递', "na'kuai'di", 4300, 'daily', 'curated-daily'),
    ('纳兰性德', "na'lan'xing'de", 3900, 'person', 'curated-person'),
    ('拿铁咖啡', "na'tie'ka'fei", 4300, 'daily', 'curated-daily'),
    ('南仁东', "nan'ren'dong", 2800, 'person', 'curated-person'),
    ('脑卒中', "nao'cu'zhong", 4300, 'medical', 'curated-medical'),
    ('脑裂现象', "nao'lie'xian'xiang", 3300, 'it', 'curated-it'),
    ('脑血管畸形', "nao'xue'guan'ji'xing", 3100, 'medical', 'curated-medical'),
    ('内存屏障', "nei'cun'ping'zhang", 3600, 'it', 'curated-it'),
    ('内存映射文件', "nei'cun'ying'she'wen'jian", 3500, 'it', 'curated-it'),
    ('内分泌治疗', "nei'fen'mi'zhi'liao", 3800, 'medical', 'curated-medical'),
    ('逆向工程', "ni'xiang'gong'cheng", 3800, 'it', 'curated-it'),
    ('年度考核', "nian'du'kao'he", 3500, 'daily', 'curated-daily'),
    ('年夜饭', "nian'ye'fan", 3800, 'daily', 'curated-daily'),
    ('尿常规', "niao'chang'gui", 4400, 'medical', 'curated-medical'),
    ('脓毒性休克', "nong'du'xing'xiu'ke", 3600, 'medical', 'curated-medical'),
    ('脓毒症', "nong'du'zheng", 3800, 'medical', 'curated-medical'),
    ('诺特', "nuo'te", 1900, 'person', 'curated-person'),
    ('疟疾', "nv'ji", 3900, 'medical', 'curated-medical'),
    ('欧几里得', "ou'ji'li'de", 2900, 'person', 'curated-person'),
    ('欧拉', "ou'la", 3200, 'person', 'curated-person'),
    ('欧阳修', "ou'yang'xiu", 3400, 'person', 'curated-person'),
    ('帕金森病', "pa'jin'sen'bing", 4100, 'medical', 'curated-medical'),
    ('帕斯卡', "pa'si'ka", 2800, 'person', 'curated-person'),
    ('排除合理怀疑', "pai'chu'he'li'huai'yi", 3800, 'law', 'curated-law'),
    ('潘安仁', "pan'an'ren", 850, 'person', 'curated-person'),
    ('潘承洞', "pan'cheng'dong", 1900, 'person', 'curated-person'),
    ('潘建伟', "pan'jian'wei", 2800, 'person', 'curated-person'),
    ('潘天寿', "pan'tian'shou", 2800, 'person', 'curated-person'),
    ('泮托拉唑', "pan'tuo'la'zuo", 3900, 'medical', 'curated-medical'),
    ('潘玉良', "pan'yu'liang", 2200, 'person', 'curated-person'),
    ('庞加莱', "pang'jia'lai", 2500, 'person', 'curated-person'),
    ('庞士元', "pang'shi'yuan", 950, 'person', 'curated-person'),
    ('泡利', "pao'li", 2200, 'person', 'curated-person'),
    ('泡温泉', "pao'wen'quan", 3400, 'daily', 'curated-daily'),
    ('配对交易', "pei'dui'jiao'yi", 3600, 'finance', 'curated-finance'),
    ('彭桓武', "peng'huan'wu", 1500, 'person', 'curated-person'),
    ('彭罗斯', "peng'luo'si", 2600, 'person', 'curated-person'),
    ('批量归一化', "pi'liang'gui'yi'hua", 3600, 'it', 'curated-it'),
    ('偏瘫', "pian'tan", 3300, 'medical', 'curated-medical'),
    ('拼单', "pin'dan", 3800, 'daily', 'curated-daily'),
    ('破产重整', "po'chan'chong'zheng", 4100, 'law', 'curated-law'),
    ('破产管理人', "po'chan'guan'li'ren", 3900, 'law', 'curated-law'),
    ('破产清算', "po'chan'qing'suan", 4200, 'law', 'curated-law'),
    ('破伤风抗毒素', "po'shang'feng'kang'du'su", 3800, 'medical', 'curated-medical'),
    ('普鲁斯特', "pu'lu'si'te", 2700, 'person', 'curated-person'),
    ('朴素贝叶斯', "pu'su'bei'ye'si", 3800, 'it', 'curated-it'),
    ('普通合伙人', "pu'tong'he'huo'ren", 3900, 'law', 'curated-law'),
    ('普希金', "pu'xi'jin", 3200, 'person', 'curated-person'),
    ('齐白石', "qi'bai'shi", 4500, 'person', 'curated-person'),
    ('戚发轫', "qi'fa'ren", 2200, 'person', 'curated-person'),
    ('契诃夫', "qi'he'fu", 3100, 'person', 'curated-person'),
    ('气泡水', "qi'pao'shui", 3900, 'daily', 'curated-daily'),
    ('期望薪资', "qi'wang'xin'zi", 3500, 'daily', 'curated-daily'),
    ('钱德拉塞卡', "qian'de'la'sai'ka", 1650, 'person', 'curated-person'),
    ('前馈神经网络', "qian'kui'shen'jing'wang'luo", 3600, 'it', 'curated-it'),
    ('钱临照', "qian'lin'zhao", 890, 'person', 'curated-person'),
    ('钱穆', "qian'mu", 3400, 'person', 'curated-person'),
    ('钱三强', "qian'san'qiang", 2400, 'person', 'curated-person'),
    ('钱伟长', "qian'wei'chang", 2200, 'person', 'curated-person'),
    ('前向传播', "qian'xiang'chuan'bo", 3900, 'it', 'curated-it'),
    ('钱乙', "qian'yi", 710, 'person', 'curated-person'),
    ('钱钟书', "qian'zhong'shu", 3800, 'person', 'curated-person'),
    ('前缀树', "qian'zhui'shu", 3400, 'it', 'curated-it'),
    ('强一致性', "qiang'yi'zhi'xing", 3800, 'it', 'curated-it'),
    ('强直性脊柱炎', "qiang'zhi'xing'ji'zhu'yan", 3900, 'medical', 'curated-medical'),
    ('桥本甲状腺炎', "qiao'ben'jia'zhuang'xian'yan", 3700, 'medical', 'curated-medical'),
    ('乔吉', "qiao'ji", 1600, 'person', 'curated-person'),
    ('乔伊斯', "qiao'yi'si", 2800, 'person', 'curated-person'),
    ('秦观', "qin'guan", 2800, 'person', 'curated-person'),
    ('请病假', "qing'bing'jia", 3400, 'daily', 'curated-daily'),
    ('请年假', "qing'nian'jia", 3500, 'daily', 'curated-daily'),
    ('情势变更', "qing'shi'bian'geng", 3700, 'law', 'curated-law'),
    ('请事假', "qing'shi'jia", 3300, 'daily', 'curated-daily'),
    ('清算优先权', "qing'suan'you'xian'quan", 3600, 'law', 'curated-law'),
    ('丘成桐', "qiu'cheng'tong", 3600, 'person', 'curated-person'),
    ('秋瑾', "qiu'jin", 2800, 'person', 'curated-person'),
    ('仇英', "qiu'ying", 1900, 'person', 'curated-person'),
    ('取保候审', "qu'bao'hou'shen", 4300, 'law', 'curated-law'),
    ('取件码', "qu'jian'ma", 4100, 'daily', 'curated-daily'),
    ('全参数微调', "quan'can'shu'wei'tiao", 3500, 'it', 'curated-it'),
    ('全外显子组测序', "quan'wai'xian'zi'zu'ce'xu", 3600, 'medical', 'curated-medical'),
    ('权重量化', "quan'zhong'liang'hua", 3600, 'it', 'curated-it'),
    ('权重衰减', "quan'zhong'shuai'jian", 3400, 'it', 'curated-it'),
    ('确实如此', "que'shi'ru'ci", 3700, 'daily', 'curated-daily'),
    ('缺血性脑卒中', "que'xue'xing'nao'cu'zhong", 3700, 'medical', 'curated-medical'),
    ('认购期权', "ren'gou'qi'quan", 3700, 'finance', 'curated-finance'),
    ('认沽期权', "ren'gu'qi'quan", 3600, 'finance', 'curated-finance'),
    ('人情世故', "ren'qing'shi'gu", 3500, 'daily', 'curated-daily'),
    ('任新民', "ren'xin'min", 1450, 'person', 'curated-person'),
    ('容错机制', "rong'cuo'ji'zhi", 3700, 'it', 'curated-it'),
    ('容器编排', "rong'qi'bian'pai", 3800, 'it', 'curated-it'),
    ('柔石', "rou'shi", 1400, 'person', 'curated-person'),
    ('茹志鹃', "ru'zhi'juan", 1250, 'person', 'curated-person'),
    ('阮籍', "ruan'ji", 1500, 'person', 'curated-person'),
    ('阮咸', "ruan'xian", 530, 'person', 'curated-person'),
    ('萨缪尔森', "sa'miu'er'sen", 2200, 'person', 'curated-person'),
    ('萨特', "sa'te", 2800, 'person', 'curated-person'),
    ('塞来昔布', "sai'lai'xi'bu", 3800, 'medical', 'curated-medical'),
    ('三叉神经痛', "san'cha'shen'jing'tong", 3600, 'medical', 'curated-medical'),
    ('散散步', "san'san'bu", 3300, 'daily', 'curated-daily'),
    ('桑弘羊', "sang'hong'yang", 1200, 'person', 'curated-person'),
    ('僧一行', "seng'yi'xing", 780, 'person', 'curated-person'),
    ('晒太阳', "shai'tai'yang", 3600, 'daily', 'curated-daily'),
    ('山涛', "shan'tao", 650, 'person', 'curated-person'),
    ('上班族', "shang'ban'zu", 3900, 'daily', 'curated-daily'),
    ('上官婉儿', "shang'guan'wan'er", 2700, 'person', 'curated-person'),
    ('尚小云', "shang'xiao'yun", 1800, 'person', 'curated-person'),
    ('商业秘密保护', "shang'ye'mi'mi'bao'hu", 3800, 'law', 'curated-law'),
    ('哨兵模式', "shao'bing'mo'shi", 3700, 'it', 'curated-it'),
    ('稍等一下', "shao'deng'yi'xia", 3400, 'daily', 'curated-daily'),
    ('社保缴纳', "she'bao'jiao'na", 3700, 'daily', 'curated-daily'),
    ('社会招聘', "she'hui'zhao'pin", 3600, 'daily', 'curated-daily'),
    ('谁说不是呢', "shei'shuo'bu'shi'ne", 2700, 'daily', 'curated-daily'),
    ('谁知道呢', "shei'zhi'dao'ne", 3600, 'daily', 'curated-daily'),
    ('申不害', "shen'bu'hai", 760, 'person', 'curated-person'),
    ('慎到', "shen'dao", 680, 'person', 'curated-person'),
    ('深度订单簿', "shen'du'ding'dan'bu", 3600, 'finance', 'curated-finance'),
    ('深度神经网络', "shen'du'shen'jing'wang'luo", 4200, 'it', 'curated-it'),
    ('神经机器翻译', "shen'jing'ji'qi'fan'yi", 3600, 'it', 'curated-it'),
    ('伸懒腰', "shen'lan'yao", 2600, 'daily', 'curated-daily'),
    ('实体对齐', "shen'ti'dui'qi", 3100, 'it', 'curated-it'),
    ('肾小球滤过率', "shen'xiao'qiu'lv'guo'lv", 3700, 'medical', 'curated-medical'),
    ('沈雁冰', "shen'yan'bing", 1350, 'person', 'curated-person'),
    ('深紫外光刻', "shen'zi'wai'guang'ke", 3700, 'it', 'curated-it'),
    ('生成对抗网络', "sheng'cheng'dui'kang'wang'luo", 3600, 'it', 'curated-it'),
    ('生煎包', "sheng'jian'bao", 3800, 'daily', 'curated-daily'),
    ('省考', "sheng'kao", 3900, 'daily', 'curated-daily'),
    ('生椰拿铁', "sheng'ye'na'tie", 4100, 'daily', 'curated-daily'),
    ('十二指肠溃疡', "shi'er'zhi'chang'kui'yang", 4100, 'medical', 'curated-medical'),
    ('实话实说', "shi'hua'shi'shuo", 3800, 'daily', 'curated-daily'),
    ('事件驱动架构', "shi'jian'qu'dong'jia'gou", 3700, 'it', 'curated-it'),
    ('市净率', "shi'jing'lv", 4300, 'finance', 'curated-finance'),
    ('适可而止', "shi'ke'er'zhi", 3100, 'daily', 'curated-daily'),
    ('史可法', "shi'ke'fa", 1600, 'person', 'curated-person'),
    ('拭目以待', "shi'mu'yi'dai", 3500, 'daily', 'curated-daily'),
    ('视频生成', "shi'pin'sheng'cheng", 4100, 'it', 'curated-it'),
    ('十三薪', "shi'san'xin", 3200, 'daily', 'curated-daily'),
    ('事实性核查', "shi'shi'xing'he'cha", 3200, 'it', 'curated-it'),
    ('史铁生', "shi'tie'sheng", 3500, 'person', 'curated-person'),
    ('实习证明', "shi'xi'zheng'ming", 3300, 'daily', 'curated-daily'),
    ('失信被执行人', "shi'xin'bei'zhi'xing'ren", 4200, 'law', 'curated-law'),
    ('事业编', "shi'ye'bian", 4200, 'daily', 'curated-daily'),
    ('施一公', "shi'yi'gong", 3100, 'person', 'curated-person'),
    ('试用期', "shi'yong'qi", 4100, 'daily', 'curated-daily'),
    ('失语症', "shi'yu'zheng", 3200, 'medical', 'curated-medical'),
    ('手冲咖啡', "shou'chong'ka'fei", 3700, 'daily', 'curated-daily'),
    ('首次公开募股', "shou'ci'gong'kai'mu'gu", 4100, 'finance', 'curated-finance'),
    ('收益率曲线倒挂', "shou'yi'lv'qu'xian'dao'gua", 3600, 'finance', 'curated-finance'),
    ('叔本华', "shu'ben'hua", 2700, 'person', 'curated-person'),
    ('数据并行', "shu'ju'bing'xing", 3700, 'it', 'curated-it'),
    ('舒庆春', "shu'qing'chun", 1200, 'person', 'curated-person'),
    ('舒婷', "shu'ting", 2600, 'person', 'curated-person'),
    ('束星北', "shu'xing'bei", 1550, 'person', 'curated-person'),
    ('述职报告', "shu'zhi'bao'gao", 3700, 'daily', 'curated-daily'),
    ('述职答辩', "shu'zhi'da'bian", 3200, 'daily', 'curated-daily'),
    ('刷朋友圈', "shua'peng'you'quan", 4100, 'daily', 'curated-daily'),
    ('刷视频', "shua'shi'pin", 4300, 'daily', 'curated-daily'),
    ('刷微博', "shua'wei'bo", 3700, 'daily', 'curated-daily'),
    ('双氯芬酸钠', "shuang'lv'fen'suan'na", 4100, 'medical', 'curated-medical'),
    ('双一流', "shuang'yi'liu", 3900, 'daily', 'curated-daily'),
    ('税后薪资', "shui'hou'xin'zi", 3400, 'daily', 'curated-daily'),
    ('睡懒觉', "shui'lan'jiao", 3500, 'daily', 'curated-daily'),
    ('水平拆分', "shui'ping'chai'fen", 3400, 'it', 'curated-it'),
    ('税前薪资', "shui'qian'xin'zi", 3400, 'daily', 'curated-daily'),
    ('顺其自然', "shun'qi'zi'ran", 3800, 'daily', 'curated-daily'),
    ('说的也是', "shuo'de'ye'shi", 3100, 'daily', 'curated-daily'),
    ('说话算话', "shuo'hua'suan'hua", 3500, 'daily', 'curated-daily'),
    ('说来话长', "shuo'lai'hua'chang", 3700, 'daily', 'curated-daily'),
    ('斯宾诺莎', "si'bin'nuo'sha", 2400, 'person', 'curated-person'),
    ('死代码消除', "si'dai'ma'xiao'chu", 3100, 'it', 'curated-it'),
    ('司马师', "si'ma'shi", 1450, 'person', 'curated-person'),
    ('司马昭', "si'ma'zhao", 2100, 'person', 'curated-person'),
    ('司马仲达', "si'ma'zhong'da", 1300, 'person', 'curated-person'),
    ('司马子长', "si'ma'zi'chang", 320, 'person', 'curated-person'),
    ('私募基金', "si'mu'ji'jin", 4500, 'finance', 'curated-finance'),
    ('死锁避免', "si'suo'bi'mian", 3400, 'it', 'curated-it'),
    ('死锁检测', "si'suo'jian'ce", 3500, 'it', 'curated-it'),
    ('司汤达', "si'tang'da", 2400, 'person', 'curated-person'),
    ('宋慈', "song'ci", 1450, 'person', 'curated-person'),
    ('送货上门', "song'huo'shang'men", 3800, 'daily', 'curated-daily'),
    ('宋应星', "song'ying'xing", 1800, 'person', 'curated-person'),
    ('宋玉', "song'yu", 1500, 'person', 'curated-person'),
    ('搜索增强', "sou'suo'zeng'qiang", 3600, 'it', 'curated-it'),
    ('苏步青', "su'bu'qing", 1900, 'person', 'curated-person'),
    ('苏童', "su'tong", 2400, 'person', 'curated-person'),
    ('苏洵', "su'xun", 2800, 'person', 'curated-person'),
    ('苏辙', "su'zhe", 2900, 'person', 'curated-person'),
    ('苏子瞻', "su'zi'zhan", 420, 'person', 'curated-person'),
    ('随便你', "sui'bian'ni", 3700, 'daily', 'curated-daily'),
    ('随份子', "sui'fen'zi", 3200, 'daily', 'curated-daily'),
    ('随机森林', "sui'ji'sen'lin", 4200, 'it', 'curated-it'),
    ('随机梯度下降', "sui'ji'ti'du'xia'jiang", 3900, 'it', 'curated-it'),
    ('随机应变', "sui'ji'ying'bian", 3200, 'daily', 'curated-daily'),
    ('随时联系', "sui'shi'lian'xi", 3100, 'daily', 'curated-daily'),
    ('孙家栋', "sun'jia'dong", 1900, 'person', 'curated-person'),
    ('孙犁', "sun'li", 1900, 'person', 'curated-person'),
    ('索恩', "suo'en", 1900, 'person', 'curated-person'),
    ('缩放点积注意力', "suo'fang'dian'ji'zhu'yi'li", 3200, 'it', 'curated-it'),
    ('所有者权益变动表', "suo'you'zhe'quan'yi'bian'dong'biao", 3700, 'finance', 'curated-finance'),
    ('泰勒斯', "tai'le'si", 1900, 'person', 'curated-person'),
    ('谭富英', "tan'fu'ying", 1500, 'person', 'curated-person'),
    ('碳化硅', "tan'hua'gui", 4100, 'it', 'curated-it'),
    ('谈家桢', "tan'jia'zhen", 1650, 'person', 'curated-person'),
    ('澹台灭明', "tan'tai'mie'ming", 1900, 'person', 'curated-person'),
    ('谭铁牛', "tan'tie'niu", 2400, 'person', 'curated-person'),
    ('汤飞凡', "tang'fei'fan", 1700, 'person', 'curated-person'),
    ('汤姆孙', "tang'mu'sun", 2100, 'person', 'curated-person'),
    ('糖尿病肾病', "tang'niao'bing'shen'bing", 3800, 'medical', 'curated-medical'),
    ('糖尿病酮症酸中毒', "tang'niao'bing'tong'zheng'suan'zhong'du", 3600, 'medical', 'curated-medical'),
    ('堂食', "tang'shi", 3300, 'daily', 'curated-daily'),
    ('汤显祖', "tang'xian'zu", 890, 'person', 'curated-person'),
    ('汤晓鸥', "tang'xiao'ou", 3100, 'person', 'curated-person'),
    ('套期保值', "tao'qi'bao'zhi", 3900, 'finance', 'curated-finance'),
    ('陶渊明', "tao'yuan'ming", 4300, 'person', 'curated-person'),
    ('陶哲轩', "tao'zhe'xuan", 3800, 'person', 'curated-person'),
    ('特发性肺纤维化', "te'fa'xing'fei'xian'wei'hua", 3200, 'medical', 'curated-medical'),
    ('特应性皮炎', "te'ying'xing'pi'yan", 3700, 'medical', 'curated-medical'),
    ('特征归一化', "te'zheng'gui'yi'hua", 3700, 'it', 'curated-it'),
    ('梯度爆炸', "ti'du'bao'zha", 3700, 'it', 'curated-it'),
    ('梯度裁剪', "ti'du'cai'jian", 3200, 'it', 'curated-it'),
    ('梯度消失', "ti'du'xiao'shi", 3800, 'it', 'curated-it'),
    ('提示微调', "ti'shi'wei'tiao", 3500, 'it', 'curated-it'),
    ('条件随机场', "tiao'jian'sui'ji'chang", 3600, 'it', 'curated-it'),
    ('调休单', "tiao'xiu'dan", 2700, 'daily', 'curated-daily'),
    ('调休假', "tiao'xiu'jia", 3200, 'daily', 'curated-daily'),
    ('听你的', "ting'ni'de", 3800, 'daily', 'curated-daily'),
    ('童第周', "tong'di'zhou", 2200, 'person', 'curated-person'),
    ('统计套利', "tong'ji'tao'li", 3800, 'finance', 'curated-finance'),
    ('通勤', "tong'qin", 4100, 'daily', 'curated-daily'),
    ('同时履行抗辩权', "tong'shi'lv'xing'kang'bian'quan", 3600, 'law', 'curated-law'),
    ('通宵达旦', "tong'xiao'da'dan", 3200, 'daily', 'curated-daily'),
    ('同业存单', "tong'ye'cun'dan", 4100, 'finance', 'curated-finance'),
    ('统一内存架构', "tong'yi'nei'cun'jia'gou", 3600, 'it', 'curated-it'),
    ('统招', "tong'zhao", 3200, 'daily', 'curated-daily'),
    ('投简历', "tou'jian'li", 4200, 'daily', 'curated-daily'),
    ('头脑风暴', "tou'nao'feng'bao", 4100, 'daily', 'curated-daily'),
    ('突发奇想', "tu'fa'qi'xiang", 3500, 'daily', 'curated-daily'),
    ('突发性耳聋', "tu'fa'xing'er'long", 3600, 'medical', 'curated-medical'),
    ('屠格涅夫', "tu'ge'nie'fu", 2700, 'person', 'curated-person'),
    ('图神经网络', "tu'shen'jing'wang'luo", 3900, 'it', 'curated-it'),
    ('屠守锷', "tu'shou'e", 1200, 'person', 'curated-person'),
    ('图像生成', "tu'xiang'sheng'cheng", 4200, 'it', 'curated-it'),
    ('团圆饭', "tuan'yuan'fan", 3400, 'daily', 'curated-daily'),
    ('退换货', "tui'huan'huo", 3700, 'daily', 'curated-daily'),
    ('拓跋珪', "tuo'ba'gui", 2100, 'person', 'curated-person'),
    ('托勒密', "tuo'le'mi", 2200, 'person', 'curated-person'),
    ('陀思妥耶夫斯基', "tuo'si'tuo'ye'fu'si'ji", 3400, 'person', 'curated-person'),
    ('外观设计专利', "wai'guan'she'ji'zhuan'li", 4100, 'law', 'curated-law'),
    ('晚高峰', "wan'gao'feng", 3800, 'daily', 'curated-daily'),
    ('腕管综合征', "wan'guan'zong'he'zheng", 3400, 'medical', 'curated-medical'),
    ('万家宝', "wan'jia'bao", 980, 'person', 'curated-person'),
    ('完美转发', "wan'mei'zhuan'fa", 3400, 'it', 'curated-it'),
    ('完颜阿骨打', "wan'yan'a'gu'da", 2500, 'person', 'curated-person'),
    ('完颜亮', "wan'yan'liang", 1950, 'person', 'curated-person'),
    ('王安石', "wang'an'shi", 3800, 'person', 'curated-person'),
    ('王贲', "wang'ben", 780, 'person', 'curated-person'),
    ('王勃', "wang'bo", 3100, 'person', 'curated-person'),
    ('王昌龄', "wang'chang'ling", 2200, 'person', 'curated-person'),
    ('王大珩', "wang'da'heng", 1550, 'person', 'curated-person'),
    ('王夫之', "wang'fu'zhi", 1750, 'person', 'curated-person'),
    ('王淦昌', "wang'gan'chang", 1750, 'person', 'curated-person'),
    ('王戎', "wang'rong", 610, 'person', 'curated-person'),
    ('王实甫', "wang'shi'fu", 560, 'person', 'curated-person'),
    ('王维', "wang'wei", 4100, 'person', 'curated-person'),
    ('王希季', "wang'xi'ji", 1400, 'person', 'curated-person'),
    ('王小波', "wang'xiao'bo", 3700, 'person', 'curated-person'),
    ('王小谟', "wang'xiao'mo", 2100, 'person', 'curated-person'),
    ('王元', "wang'yuan", 2400, 'person', 'curated-person'),
    ('汪曾祺', "wang'zeng'qi", 3400, 'person', 'curated-person'),
    ('胃肠镜', "wei'chang'jing", 4200, 'medical', 'curated-medical'),
    ('维纳', "wei'na", 2100, 'person', 'curated-person'),
    ('萎缩性胃炎', "wei'suo'xing'wei'yan", 3800, 'medical', 'curated-medical'),
    ('魏源', "wei'yuan", 2200, 'person', 'curated-person'),
    ('位置编码', "wei'zhi'bian'ma", 3600, 'it', 'curated-it'),
    ('文献综述', "wen'xian'zong'shu", 3800, 'daily', 'curated-daily'),
    ('稳扎稳打', "wen'zha'wen'da", 3400, 'daily', 'curated-daily'),
    ('沃森', "wo'sen", 2400, 'person', 'curated-person'),
    ('吴大猷', "wu'da'you", 1450, 'person', 'curated-person'),
    ('吴冠中', "wu'guan'zhong", 2800, 'person', 'curated-person'),
    ('无过错责任', "wu'guo'cuo'ze'ren", 3800, 'law', 'curated-law'),
    ('无监督预训练', "wu'jian'du'yu'xun'lian", 3600, 'it', 'curated-it'),
    ('伍连德', "wu'lian'de", 2600, 'person', 'curated-person'),
    ('物美价廉', "wu'mei'jia'lian", 3900, 'daily', 'curated-daily'),
    ('吴孟超', "wu'meng'chao", 2200, 'person', 'curated-person'),
    ('无权代理', "wu'quan'dai'li", 3700, 'law', 'curated-law'),
    ('无锁队列', "wu'suo'dui'lie", 3600, 'it', 'curated-it'),
    ('无所谓', "wu'suo'wei", 4300, 'daily', 'curated-daily'),
    ('吴文俊', "wu'wen'jun", 2800, 'person', 'curated-person'),
    ('五险一金', "wu'xian'yi'jin", 4600, 'daily', 'curated-daily'),
    ('吴有训', "wu'you'xun", 1250, 'person', 'curated-person'),
    ('希尔伯特', "xi'er'bo'te", 2600, 'person', 'curated-person'),
    ('席勒', "xi'le", 2500, 'person', 'curated-person'),
    ('稀疏检索', "xi'shu'jian'suo", 2700, 'it', 'curated-it'),
    ('系统性风险', "xi'tong'xing'feng'xian", 3900, 'finance', 'curated-finance'),
    ('系统性红斑狼疮', "xi'tong'xing'hong'ban'lang'chuang", 4200, 'medical', 'curated-medical'),
    ('下厨做饭', "xia'chu'zuo'fan", 3500, 'daily', 'curated-daily'),
    ('下次一定', "xia'ci'yi'ding", 3300, 'daily', 'curated-daily'),
    ('夏侯惇', "xia'hou'dun", 2200, 'person', 'curated-person'),
    ('夏侯妙才', "xia'hou'miao'cai", 860, 'person', 'curated-person'),
    ('夏侯渊', "xia'hou'yuan", 1950, 'person', 'curated-person'),
    ('夏侯元让', "xia'hou'yuan'rang", 880, 'person', 'curated-person'),
    ('下肢深静脉血栓', "xia'zhi'shen'jing'mai'xue'shuan", 3500, 'medical', 'curated-medical'),
    ('先进封装', "xian'jin'feng'zhuang", 3800, 'it', 'curated-it'),
    ('现金流量表', "xian'jin'liu'liang'biao", 4400, 'finance', 'curated-finance'),
    ('现金流折现', "xian'jin'liu'zhe'xian", 3800, 'finance', 'curated-finance'),
    ('线上监控', "xian'shang'jian'kong", 3900, 'daily', 'curated-daily'),
    ('冼星海', "xian'xing'hai", 2900, 'person', 'curated-person'),
    ('限制高消费', "xian'zhi'gao'xiao'fei", 4100, 'law', 'curated-law'),
    ('相对位置编码', "xiang'dui'wei'zhi'bian'ma", 3200, 'it', 'curated-it'),
    ('想多了', "xiang'duo'liao", 3400, 'daily', 'curated-daily'),
    ('向量检索', "xiang'liang'jian'suo", 3600, 'it', 'curated-it'),
    ('向量时钟', "xiang'liang'shi'zhong", 3100, 'it', 'curated-it'),
    ('向量数据库', "xiang'liang'shu'ju'ku", 3900, 'it', 'curated-it'),
    ('香农', "xiang'nong", 2900, 'person', 'curated-person'),
    ('向秀', "xiang'xiu", 580, 'person', 'curated-person'),
    ('消费者权益保护法', "xiao'fei'zhe'quan'yi'bao'hu'fa", 4300, 'law', 'curated-law'),
    ('萧红', "xiao'hong", 2900, 'person', 'curated-person'),
    ('消化性溃疡', "xiao'hua'xing'kui'yang", 4200, 'medical', 'curated-medical'),
    ('萧军', "xiao'jun", 1600, 'person', 'curated-person'),
    ('效力待定', "xiao'li'dai'ding", 3600, 'law', 'curated-law'),
    ('小笼包', "xiao'long'bao", 4200, 'daily', 'curated-daily'),
    ('萧统', "xiao'tong", 1900, 'person', 'curated-person'),
    ('消息中间件', "xiao'xi'zhong'jian'jian", 3900, 'it', 'curated-it'),
    ('校园招聘', "xiao'yuan'zhao'pin", 3800, 'daily', 'curated-daily'),
    ('协程调度器', "xie'cheng'diao'du'qi", 3600, 'it', 'curated-it'),
    ('谢灵运', "xie'ling'yun", 2600, 'person', 'curated-person'),
    ('协同办公', "xie'tong'ban'gong", 3800, 'daily', 'curated-daily'),
    ('谢希德', "xie'xi'de", 1400, 'person', 'curated-person'),
    ('谢玄晖', "xie'xuan'hui", 920, 'person', 'curated-person'),
    ('心房颤动', "xin'fang'chan'dong", 4100, 'medical', 'curated-medical'),
    ('新辅助化疗', "xin'fu'zhu'hua'liao", 3600, 'medical', 'curated-medical'),
    ('心肌梗死', "xin'ji'geng'si", 4500, 'medical', 'curated-medical'),
    ('心里有数', "xin'li'you'shu", 3400, 'daily', 'curated-daily'),
    ('心室颤动', "xin'shi'chan'dong", 3700, 'medical', 'curated-medical'),
    ('心跳检测', "xin'tiao'jian'ce", 3600, 'it', 'curated-it'),
    ('辛幼安', "xin'you'an", 310, 'person', 'curated-person'),
    ('刑法典', "xing'fa'dian", 3900, 'law', 'curated-law'),
    ('性价比', "xing'jia'bi", 4600, 'daily', 'curated-daily'),
    ('刑事拘留', "xing'shi'ju'liu", 4100, 'law', 'curated-law'),
    ('行为保全', "xing'wei'bao'quan", 3500, 'law', 'curated-law'),
    ('行政处罚法', "xing'zheng'chu'fa'fa", 4100, 'law', 'curated-law'),
    ('行政复议', "xing'zheng'fu'yi", 4200, 'law', 'curated-law'),
    ('行政诉讼', "xing'zheng'su'song", 4100, 'law', 'curated-law'),
    ('行政许可法', "xing'zheng'xu'ke'fa", 3900, 'law', 'curated-law'),
    ('熊彼特', "xiong'bi'te", 2300, 'person', 'curated-person'),
    ('休谟', "xiu'mo", 2300, 'person', 'curated-person'),
    ('许宝騄', "xu'bao'lu", 920, 'person', 'curated-person'),
    ('徐悲鸿', "xu'bei'hong", 4400, 'person', 'curated-person'),
    ('徐霞客', "xu'xia'ke", 2600, 'person', 'curated-person'),
    ('徐元直', "xu'yuan'zhi", 880, 'person', 'curated-person'),
    ('旋转位置编码', "xuan'zhuan'wei'zhi'bian'ma", 3400, 'it', 'curated-it'),
    ('血常规', "xue'chang'gui", 4600, 'medical', 'curated-medical'),
    ('血管性痴呆', "xue'guan'xing'chi'dai", 3300, 'medical', 'curated-medical'),
    ('薛稷', "xue'ji", 1600, 'person', 'curated-person'),
    ('雪莱', "xue'lai", 2700, 'person', 'curated-person'),
    ('薛其坤', "xue'qi'kun", 2400, 'person', 'curated-person'),
    ('学习率调度', "xue'xi'lv'diao'du", 3300, 'it', 'curated-it'),
    ('循环神经网络', "xun'huan'shen'jing'wang'luo", 4100, 'it', 'curated-it'),
    ('荀慧生', "xun'hui'sheng", 1850, 'person', 'curated-person'),
    ('荀况', "xun'kuang", 1200, 'person', 'curated-person'),
    ('荀文若', "xun'wen'ruo", 1100, 'person', 'curated-person'),
    ('循序渐进', "xun'xu'jian'jin", 3800, 'daily', 'curated-daily'),
    ('亚当斯密', "ya'dang'si'mi", 2700, 'person', 'curated-person'),
    ('压马路', "ya'ma'lu", 2900, 'daily', 'curated-daily'),
    ('压岁钱', "ya'sui'qian", 3600, 'daily', 'curated-daily'),
    ('严凤英', "yan'feng'ying", 1900, 'person', 'curated-person'),
    ('严复', "yan'fu", 2400, 'person', 'curated-person'),
    ('言归正传', "yan'gui'zheng'zhuan", 3500, 'daily', 'curated-daily'),
    ('严济慈', "yan'ji'ci", 1400, 'person', 'curated-person'),
    ('掩码语言模型', "yan'ma'yu'yan'mo'xing", 3300, 'it', 'curated-it'),
    ('掩码自编码器', "yan'ma'zi'bian'ma'qi", 3500, 'it', 'curated-it'),
    ('颜宁', "yan'ning", 3300, 'person', 'curated-person'),
    ('阎应元', "yan'ying'yuan", 720, 'person', 'curated-person'),
    ('严羽', "yan'yu", 1750, 'person', 'curated-person'),
    ('杨嘉墀', "yang'jia'chi", 1200, 'person', 'curated-person'),
    ('杨绛', "yang'jiang", 3600, 'person', 'curated-person'),
    ('杨炯', "yang'jiong", 820, 'person', 'curated-person'),
    ('姚期智', "yao'qi'zhi", 3400, 'person', 'curated-person'),
    ('姚桐斌', "yao'tong'bin", 1250, 'person', 'curated-person'),
    ('腰椎间盘突出', "yao'zhui'jian'pan'tu'chu", 4400, 'medical', 'curated-medical'),
    ('耶律阿保机', "ye'lv'a'bao'ji", 2400, 'person', 'curated-person'),
    ('耶律楚材', "ye'lv'chu'cai", 2400, 'person', 'curated-person'),
    ('叶企孙', "ye'qi'sun", 1650, 'person', 'curated-person'),
    ('液体活检', "ye'ti'huo'jian", 3600, 'medical', 'curated-medical'),
    ('夜宵', "ye'xiao", 3800, 'daily', 'curated-daily'),
    ('异步非阻塞', "yi'bu'fei'zu'sai", 3800, 'it', 'curated-it'),
    ('异步复制', "yi'bu'fu'zhi", 3500, 'it', 'curated-it'),
    ('移动语义', "yi'dong'yu'yi", 3600, 'it', 'curated-it'),
    ('乙肝两对半', "yi'gan'liang'dui'ban", 4100, 'medical', 'curated-medical'),
    ('异构计算', "yi'gou'ji'suan", 3500, 'it', 'curated-it'),
    ('意料之外', "yi'liao'zhi'wai", 3800, 'daily', 'curated-daily'),
    ('意料之中', "yi'liao'zhi'zhong", 3900, 'daily', 'curated-daily'),
    ('依托考昔', "yi'tuo'kao'xi", 3600, 'medical', 'curated-medical'),
    ('一言为定', "yi'yan'wei'ding", 3200, 'daily', 'curated-daily'),
    ('遗赠扶养协议', "yi'zeng'fu'yang'xie'yi", 3500, 'law', 'curated-law'),
    ('一致性哈希', "yi'zhi'xing'ha'xi", 3800, 'it', 'curated-it'),
    ('银耳汤', "yin'er'tang", 3600, 'daily', 'curated-daily'),
    ('隐含波动率', "yin'han'bo'dong'lv", 3700, 'finance', 'curated-finance'),
    ('银团贷款', "yin'tuan'dai'kuan", 3800, 'finance', 'curated-finance'),
    ('应收账款保理', "ying'shou'zhang'kuan'bao'li", 3700, 'finance', 'curated-finance'),
    ('有备无患', "you'bei'wu'huan", 3700, 'daily', 'curated-daily'),
    ('优惠券', "you'hui'quan", 4500, 'daily', 'curated-daily'),
    ('幽门螺杆菌', "you'men'luo'gan'jun", 4300, 'medical', 'curated-medical'),
    ('幽门螺杆菌感染', "you'men'luo'gan'jun'gan'ran", 3900, 'medical', 'curated-medical'),
    ('有什么事', "you'shen'me'shi", 3900, 'daily', 'curated-daily'),
    ('有限合伙人', "you'xian'he'huo'ren", 3900, 'law', 'curated-law'),
    ('右值引用', "you'zhi'yin'yong", 3700, 'it', 'curated-it'),
    ('尉迟恭', "yu'chi'gong", 2100, 'person', 'curated-person'),
    ('尉迟敬德', "yu'chi'jing'de", 2300, 'person', 'curated-person'),
    ('俞大猷', "yu'da'you", 890, 'person', 'curated-person'),
    ('余华', "yu'hua", 4400, 'person', 'curated-person'),
    ('于敏', "yu'min", 2600, 'person', 'curated-person'),
    ('虞世南', "yu'shi'nan", 2400, 'person', 'curated-person'),
    ('宇文泰', "yu'wen'tai", 1400, 'person', 'curated-person'),
    ('预训练模型', "yu'xun'lian'mo'xing", 4100, 'it', 'curated-it'),
    ('语义分割', "yu'yi'fen'ge", 3800, 'it', 'curated-it'),
    ('语音合成', "yu'yin'he'cheng", 3900, 'it', 'curated-it'),
    ('语音识别', "yu'yin'shi'bie", 4300, 'it', 'curated-it'),
    ('庾子山', "yu'zi'shan", 890, 'person', 'curated-person'),
    ('元好问', "yuan'hao'wen", 2600, 'person', 'curated-person'),
    ('原来如此', "yuan'lai'ru'ci", 3900, 'daily', 'curated-daily'),
    ('元稹', "yuan'zhen", 1400, 'person', 'curated-person'),
    ('原子层沉积', "yuan'zi'ceng'chen'ji", 3500, 'it', 'curated-it'),
    ('月度考核', "yue'du'kao'he", 3200, 'daily', 'curated-daily'),
    ('运动神经元病', "yun'dong'shen'jing'yuan'bing", 3400, 'medical', 'curated-medical'),
    ('运费险', "yun'fei'xian", 4200, 'daily', 'curated-daily'),
    ('在岸人民币', "zai'an'ren'min'bi", 3800, 'finance', 'curated-finance'),
    ('臧克家', "zang'ke'jia", 1750, 'person', 'curated-person'),
    ('早点回去', "zao'dian'hui'qu", 2900, 'daily', 'curated-daily'),
    ('早高峰', "zao'gao'feng", 3900, 'daily', 'curated-daily'),
    ('怎么办', "zen'me'ban", 4400, 'daily', 'curated-daily'),
    ('怎么了', "zen'me'liao", 4200, 'daily', 'curated-daily'),
    ('怎么说', "zen'me'shuo", 4300, 'daily', 'curated-daily'),
    ('怎么样', "zen'me'yang", 4800, 'daily', 'curated-daily'),
    ('曾巩', "zeng'gong", 1100, 'person', 'curated-person'),
    ('增资扩股协议', "zeng'zi'kuo'gu'xie'yi", 3600, 'law', 'curated-law'),
    ('查德威克', "zha'de'wei'ke", 1950, 'person', 'curated-person'),
    ('查良镛', "zha'liang'yong", 2600, 'person', 'curated-person'),
    ('债权人撤销权', "zhai'quan'ren'che'xiao'quan", 3600, 'law', 'curated-law'),
    ('债权人代位权', "zhai'quan'ren'dai'wei'quan", 3600, 'law', 'curated-law'),
    ('债权人会议', "zhai'quan'ren'hui'yi", 3800, 'law', 'curated-law'),
    ('债权投资', "zhai'quan'tou'zi", 3900, 'finance', 'curated-finance'),
    ('战略配售', "zhan'lve'pei'shou", 3500, 'finance', 'curated-finance'),
    ('詹天佑', "zhan'tian'you", 3100, 'person', 'curated-person'),
    ('张大千', "zhang'da'qian", 4300, 'person', 'curated-person'),
    ('张量并行', "zhang'liang'bing'xing", 3500, 'it', 'curated-it'),
    ('张世杰', "zhang'shi'jie", 1050, 'person', 'curated-person'),
    ('章太炎', "zhang'tai'yan", 2100, 'person', 'curated-person'),
    ('张孝骞', "zhang'xiao'qian", 1300, 'person', 'curated-person'),
    ('张养浩', "zhang'yang'hao", 2100, 'person', 'curated-person'),
    ('张翼德', "zhang'yi'de", 1550, 'person', 'curated-person'),
    ('张载', "zhang'zai", 1600, 'person', 'curated-person'),
    ('招股说明书', "zhao'gu'shuo'ming'shu", 4100, 'finance', 'curated-finance'),
    ('赵九章', "zhao'jiu'zhang", 1600, 'person', 'curated-person'),
    ('昭明太子', "zhao'ming'tai'zi", 2100, 'person', 'curated-person'),
    ('赵奢', "zhao'she", 910, 'person', 'curated-person'),
    ('赵树理', "zhao'shu'li", 1700, 'person', 'curated-person'),
    ('赵忠尧', "zhao'zhong'yao", 2100, 'person', 'curated-person'),
    ('赵子龙', "zhao'zi'long", 2600, 'person', 'curated-person'),
    ('这倒也是', "zhe'dao'ye'shi", 3500, 'daily', 'curated-daily'),
    ('真不凑巧', "zhen'bu'cou'qiao", 2400, 'daily', 'curated-daily'),
    ('珍珠奶茶', "zhen'zhu'nai'cha", 4300, 'daily', 'curated-daily'),
    ('政策性金融债', "zheng'ce'xing'jin'rong'zhai", 3700, 'finance', 'curated-finance'),
    ('正当防卫', "zheng'dang'fang'wei", 4400, 'law', 'curated-law'),
    ('政府采购法', "zheng'fu'cai'gou'fa", 4100, 'law', 'curated-law'),
    ('郑光祖', "zheng'guang'zu", 1800, 'person', 'curated-person'),
    ('证据保全', "zheng'ju'bao'quan", 3700, 'law', 'curated-law'),
    ('蒸桑拿', "zheng'sang'na", 2800, 'daily', 'curated-daily'),
    ('职级评定', "zhi'ji'ping'ding", 3300, 'daily', 'curated-daily'),
    ('直接偏好优化', "zhi'jie'pian'hao'you'hua", 3400, 'it', 'curated-it'),
    ('指令微调', "zhi'ling'wei'tiao", 3800, 'it', 'curated-it'),
    ('支气管哮喘', "zhi'qi'guan'xiao'chuan", 4100, 'medical', 'curated-medical'),
    ('知识产权', "zhi'shi'chan'quan", 4600, 'law', 'curated-law'),
    ('知识库检索', "zhi'shi'ku'jian'suo", 3700, 'it', 'curated-it'),
    ('知识图谱', "zhi'shi'tu'pu", 4200, 'it', 'curated-it'),
    ('知识蒸馏', "zhi'shi'zheng'liu", 3600, 'it', 'curated-it'),
    ('质押权', "zhi'ya'quan", 3700, 'law', 'curated-law'),
    ('质押式回购', "zhi'ya'shi'hui'gou", 3800, 'finance', 'curated-finance'),
    ('质子泵抑制剂', "zhi'zi'beng'yi'zhi'ji", 3800, 'medical', 'curated-medical'),
    ('仲裁法', "zhong'cai'fa", 3700, 'law', 'curated-law'),
    ('种草', "zhong'cao", 3900, 'daily', 'curated-daily'),
    ('肿瘤突变负荷', "zhong'liu'tu'bian'fu'he", 3400, 'medical', 'curated-medical'),
    ('钟南山', "zhong'nan'shan", 4800, 'person', 'curated-person'),
    ('中期检查', "zhong'qi'jian'cha", 3600, 'daily', 'curated-daily'),
    ('中期借贷便利', "zhong'qi'jie'dai'bian'li", 3900, 'finance', 'curated-finance'),
    ('中期票据', "zhong'qi'piao'ju", 3900, 'finance', 'curated-finance'),
    ('重症肌无力', "zhong'zheng'ji'wu'li", 3500, 'medical', 'curated-medical'),
    ('种植牙', "zhong'zhi'ya", 4300, 'medical', 'curated-medical'),
    ('周邦彦', "zhou'bang'yan", 2400, 'person', 'curated-person'),
    ('周报总结', "zhou'bao'zong'jie", 3700, 'daily', 'curated-daily'),
    ('周敦颐', "zhou'dun'yi", 1900, 'person', 'curated-person'),
    ('周公瑾', "zhou'gong'jin", 1100, 'person', 'curated-person'),
    ('周光召', "zhou'guang'zhao", 1700, 'person', 'curated-person'),
    ('周建人', "zhou'jian'ren", 1100, 'person', 'curated-person'),
    ('周立波', "zhou'li'bo", 1500, 'person', 'curated-person'),
    ('周培源', "zhou'pei'yuan", 1600, 'person', 'curated-person'),
    ('周树人', "zhou'shu'ren", 2200, 'person', 'curated-person'),
    ('周信芳', "zhou'xin'fang", 1850, 'person', 'curated-person'),
    ('周毓麟', "zhou'yu'lin", 1600, 'person', 'curated-person'),
    ('周作人', "zhou'zuo'ren", 2400, 'person', 'curated-person'),
    ('主成分分析', "zhu'cheng'fen'fen'xi", 4100, 'it', 'curated-it'),
    ('主动脉瓣狭窄', "zhu'dong'mai'ban'xia'zhai", 3300, 'medical', 'curated-medical'),
    ('主动脉夹层', "zhu'dong'mai'jia'ceng", 3600, 'medical', 'curated-medical'),
    ('诸福棠', "zhu'fu'tang", 980, 'person', 'curated-person'),
    ('主父偃', "zhu'fu'yan", 920, 'person', 'curated-person'),
    ('诸葛瑾', "zhu'ge'jin", 1100, 'person', 'curated-person'),
    ('诸葛恪', "zhu'ge'ke", 860, 'person', 'curated-person'),
    ('诸葛孔明', "zhu'ge'kong'ming", 950, 'person', 'curated-person'),
    ('朱光潜', "zhu'guang'qian", 2900, 'person', 'curated-person'),
    ('朱光亚', "zhu'guang'ya", 1850, 'person', 'curated-person'),
    ('竺可桢', "zhu'ke'zhen", 2600, 'person', 'curated-person'),
    ('祝你顺利', "zhu'ni'shun'li", 2900, 'daily', 'curated-daily'),
    ('蛛网膜下腔出血', "zhu'wang'mo'xia'qiang'chu'xue", 3500, 'medical', 'curated-medical'),
    ('注意力头数', "zhu'yi'li'tou'shu", 3400, 'it', 'curated-it'),
    ('专利无效宣告', "zhuan'li'wu'xiao'xuan'gao", 3600, 'law', 'curated-law'),
    ('转正申请', "zhuan'zheng'shen'qing", 3500, 'daily', 'curated-daily'),
    ('状态机复制', "zhuang'tai'ji'fu'zhi", 3400, 'it', 'curated-it'),
    ('庄周', "zhuang'zhou", 1800, 'person', 'curated-person'),
    ('追剧', "zhui'ju", 3900, 'daily', 'curated-daily'),
    ('资本充足率', "zi'ben'chong'zu'lv", 3800, 'finance', 'curated-finance'),
    ('资本资产定价模型', "zi'ben'zi'chan'ding'jia'mo'xing", 3600, 'finance', 'curated-finance'),
    ('自编码器', "zi'bian'ma'qi", 3100, 'it', 'curated-it'),
    ('资产证券化', "zi'chan'zheng'quan'hua", 3900, 'finance', 'curated-finance'),
    ('自动求导', "zi'dong'qiu'dao", 3400, 'it', 'curated-it'),
    ('自回归模型', "zi'hui'gui'mo'xing", 3400, 'it', 'curated-it'),
    ('自监督学习', "zi'jian'du'xue'xi", 3900, 'it', 'curated-it'),
    ('字节对编码', "zi'jie'dui'bian'ma", 3300, 'it', 'curated-it'),
    ('自适应矩估计', "zi'shi'ying'ju'gu'ji", 3200, 'it', 'curated-it'),
    ('自旋锁', "zi'xuan'suo", 3600, 'it', 'curated-it'),
    ('自由现金流', "zi'you'xian'jin'liu", 3900, 'finance', 'curated-finance'),
    ('自注意力机制', "zi'zhu'yi'li'ji'zhi", 3900, 'it', 'curated-it'),
    ('宗白华', "zong'bai'hua", 2400, 'person', 'curated-person'),
    ('宗璞', "zong'pu", 1400, 'person', 'curated-person'),
    ('宗泽', "zong'ze", 950, 'person', 'curated-person'),
    ('阻生齿', "zu'sheng'chi", 3600, 'medical', 'curated-medical'),
    ('最高额抵押', "zui'gao'e'di'ya", 3500, 'law', 'curated-law'),
    ('最终一致性', "zui'zhong'yi'zhi'xing", 3900, 'it', 'curated-it'),
    ('坐公交', "zuo'gong'jiao", 3700, 'daily', 'curated-daily'),
    ('左拉', "zuo'la", 2300, 'person', 'curated-person'),
    ('左太冲', "zuo'tai'chong", 720, 'person', 'curated-person'),
]

REQUIRED_ACCEPTANCE_ENTRIES: list[tuple[str, str, int, str, str]] = [
    ("候选栏", "hou'xuan'lan", 850, "it", "modernime-curated"),
    ("剪贴板", "jian'tie'ban", 850, "it", "modernime-curated"),
    ("上下文", "shang'xia'wen", 850, "it", "modernime-curated"),
    ("设备树", "she'bei'shu", 850, "it", "modernime-curated"),
    ("驱动程序", "qu'dong'cheng'xu", 850, "it", "modernime-curated"),
    ("交叉编译", "jiao'cha'bian'yi", 850, "it", "modernime-curated"),
    ("嵌入式", "qian'ru'shi", 850, "it", "modernime-curated"),
    ("重庆", "chong'qing", 900, "place", "wanxiang-duoyin"),
    ("重阳", "chong'yang", 800, "daily", "wanxiang-duoyin"),
    ("银行", "yin'hang", 900, "finance", "wanxiang-duoyin"),
    ("音乐", "yin'yue", 900, "daily", "wanxiang-duoyin"),
    ("快乐", "kuai'le", 900, "daily", "wanxiang-duoyin"),
    ("绿色", "lv'se", 900, "daily", "wanxiang-jichu"),
    ("女儿", "nv'er", 900, "daily", "wanxiang-jichu"),
    ("略微", "lve'wei", 850, "daily", "wanxiang-jichu"),
]

PROTECTED_CANON_PINYINS: set[str] = {
    "nihao", "xiexie", "gongzuo", "xuexi", "mingtian", "jintian", "shijian",
    "shezhi", "diannao", "shurufa", "kaixin", "rengongzhineng", "ziranyuyanchuli",
    "liangzijisuan", "damoxing", "tishici", "zhinengti", "yunyuansheng",
    "rongqihua", "gaokeyong", "duxiefenli", "fenbushisuo", "tangping", "bailan",
    "saibopengke", "jiangweidaji", "dianzimuyu", "zundujiadu", "qingxujiazhi",
    "dianzan", "haodeshoudao", "xinkuliao", "meiwenti", "mashangchuli",
    "chongdianbao", "saomazhifu", "jintiantianqihenhao", "qingbangwodakaiwenjian",
    "nengbunengbangwokanxia", "jintiantianqibucuo", "weifuwu", "sisuo", "duanyan",
    "fanxiangdaili", "xiaoxiduilie", "fuzaijunheng", "chixujicheng", "lajihuishou",
    "xianchenganquan", "yilaizhuru", "shejimoshi", "zichanfuzhaibiao",
    "xianjinliuliangbiao", "lirunbiao", "zengzhishuifapiao", "xuechanggui",
    "xindiantu", "hecigongzhen", "guanxinbing", "buluofen", "amoxilin",
    "minfadian", "bukekangli", "weiyuezeren", "liandaizeren", "budangdeli",
    "wuyinguanli", "xianzhuxingchayi", "zhengtaifenbu", "huiguifenxi",
    "zhixinqujian", "maerkefulian", "gantanhao", "wenhao", "juhao", "douhao",
    "dunhao", "fenhao", "maohao", "shuminghao", "kuohao", "shenglvehao",
    "pozhehao", "yinhao", "jiahao", "jianhao", "baifenhao", "weishime",
    "sanqian", "dataikuaile", "jiating", "jiaren", "dajia", "xiatian", "tianqi",
    "tiaojian", "xiaocheng", "guangzhou", "canting", "zantong", "zhangsan",
    "zhongguo", "xuesheng", "beijing", "shanghai", "pengyou", "fangzi", "yueding",
    "kaifa", "zenmeyang", "rmb", "dollar", "sheshidu", "jiefu", "xuhao",
    "haha", "zan", "dui", "cuo", "jiantou", "daniu", "pi", "alpha", "pingfang", "wuxian",
}

PROTECTED_ABBREVS: set[str] = {"aspl", "yxyy", "zryycl", "hstz"}


def load_english_words(repo_root: Path) -> set[str]:
    eng_cpp = (repo_root / "core/src/english_dictionary.cpp").read_text(encoding="utf-8")
    words = set(re.findall(r'"([a-z]+)"', eng_cpp))
    words.update({"gara", "garag", "appl", "syst", "windo", "devel", "dev", "gnuea", "gnueabihf"})
    return words


def load_sc_dict(sc_dict_path: Path) -> tuple[set[str], set[tuple[str, str]], set[str]]:
    sc_words: set[str] = set()
    sc_keys: set[tuple[str, str]] = set()
    sc_canon_pinyins: set[str] = set()
    proc = subprocess.run(
        ["libime_pinyindict", "-d", str(sc_dict_path), "/dev/stdout"],
        check=True,
        capture_output=True,
        text=True,
    )
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3:
            w, p = parts[0], parts[1]
            sc_words.add(w)
            sc_keys.add((w, p))
            sc_canon_pinyins.add(p.replace("'", ""))
    return sc_words, sc_keys, sc_canon_pinyins


def load_annotated_table(path: Path) -> tuple[dict[tuple[str, str], tuple[int, str, str]], dict[str, list[tuple[str, int, str, str]]]]:
    data: dict[tuple[str, str], tuple[int, str, str]] = {}
    by_word: dict[str, list[tuple[str, int, str, str]]] = defaultdict(list)
    with path.open(encoding="utf-8") as f:
        next(f)
        for line in f:
            w, p, wt, cat, abbr, src, orig_py, line_no = line.rstrip("\n").split("\t")
            norm_p = pinyin_from_source(p) or p
            wt_int = int(wt)
            data[(w, norm_p)] = (wt_int, cat, src)
            by_word[w].append((norm_p, wt_int, cat, src))
    for w in by_word:
        by_word[w].sort(key=lambda x: -x[1])
    return data, by_word


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bundle-dir",
        type=Path,
        default=Path("/home/wsl/.codex/visualizations/2026/10/11/01a128b2-3675-7722-9652-7042b1669b2f/modernime-lexicon-2026-10-12"),
        help="Path to the offline lexicon research bundle",
    )
    parser.add_argument(
        "--baseline-tsv",
        type=Path,
        default=Path("data/pinyin/modernime-knowledge.tsv"),
        help="Path to baseline modernime-knowledge.tsv",
    )
    parser.add_argument(
        "--hotwords-raw",
        type=Path,
        default=Path("data/pinyin/modernime-hotwords.raw"),
        help="Path to modernime-hotwords.raw",
    )
    parser.add_argument(
        "--sc-dict",
        type=Path,
        default=Path("/usr/share/libime/sc.dict"),
        help="Path to system LibIME sc.dict",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("data/pinyin/modernime-knowledge.tsv"),
        help="Output path for normalized 6-column TSV",
    )
    parser.add_argument(
        "--raw-output",
        type=Path,
        default=Path("data/pinyin/modernime-knowledge.raw"),
        help="Output path for LibIME 3-column raw dictionary",
    )
    parser.add_argument(
        "--polyphonic-review-output",
        type=Path,
        default=Path("data/pinyin/POLYPHONIC_REVIEW.tsv"),
        help="Output path for quarantined polyphonic review manifest",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    bundle = args.bundle_dir

    eng_words = load_english_words(repo_root)
    sc_words, sc_keys, sc_canon_pinyins = load_sc_dict(args.sc_dict)

    hot_keys: set[tuple[str, str]] = set()
    hot_words: set[str] = set()
    with args.hotwords_raw.open(encoding="utf-8") as f:
        for line in f:
            if line.strip():
                w, p, _ = line.strip().split("\t")
                hot_words.add(w)
                hot_keys.add((w, p))
                sc_canon_pinyins.add(p.replace("'", ""))

    base_rows: list[tuple[str, str, int, str, str, str]] = []
    base_canon_pinyins: set[str] = set()
    with args.baseline_tsv.open(encoding="utf-8") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            w, p, freq, cat, abbr, src = line.rstrip("\n").split("\t")
            # Keep only original THUOCL and china-idiom rows as baseline seed so re-runs are idempotent
            if src == "china-idiom" or src.startswith("thuocl-"):
                base_rows.append((w, p, int(freq), cat, abbr, src))
                base_canon_pinyins.add(p.replace("'", ""))

    allowed_english_homograph_pinyins = sc_canon_pinyins | base_canon_pinyins
    base_key_set = {(w, p) for w, p, *_ in base_rows}

    wx_jichu, wx_jichu_by_w = load_annotated_table(bundle / "normalized/wanxiang/jichu.dict.tsv")
    wx_duoyin, wx_duoyin_by_w = load_annotated_table(bundle / "normalized/wanxiang/duoyin.dict.tsv")
    ri_base, ri_base_by_w = load_annotated_table(bundle / "normalized/rime-ice/base.dict.tsv")
    ri_ext, ri_ext_by_w = load_annotated_table(bundle / "normalized/rime-ice/ext.dict.tsv")
    cedict, cedict_by_w = load_annotated_table(bundle / "normalized/cc-cedict/cedict.txt.tsv")

    jieba_dict: dict[str, tuple[int, str]] = {}
    for rel in ("normalized/jieba/dict.pending.tsv", "normalized/jieba/dict.txt.pending.tsv"):
        with (bundle / rel).open(encoding="utf-8") as f:
            next(f)
            for line in f:
                w, wt, cat, src, pos, _ = line.rstrip("\n").split("\t")
                wt_int = int(wt)
                if w not in jieba_dict or wt_int > jieba_dict[w][0]:
                    jieba_dict[w] = (wt_int, pos)

    wx_domains: dict[tuple[str, str], tuple[int, str, str]] = {}
    wx_domain_by_w: dict[str, list[tuple[str, int, str, str]]] = defaultdict(list)
    domain_files = [
        ("normalized/wanxiang/mingren.dict.tsv", "person", "wanxiang-mingren"),
        ("normalized/wanxiang/renming.dict.tsv", "person", "wanxiang-renming"),
        ("normalized/wanxiang/diming.dict.tsv", "place", "wanxiang-diming"),
        ("normalized/wanxiang/yixue.dict.tsv", "medical", "wanxiang-yixue"),
        ("normalized/wanxiang/yaopin.dict.tsv", "medical", "wanxiang-yaopin"),
        ("normalized/wanxiang/huaxue.dict.tsv", "chemistry", "wanxiang-huaxue"),
        ("normalized/wanxiang/wuzhong.dict.tsv", "biology", "wanxiang-wuzhong"),
    ]
    for rel_path, mapped_cat, mapped_src in domain_files:
        d, _ = load_annotated_table(bundle / rel_path)
        for (w, p), (wt_int, _, _) in d.items():
            if (w, p) not in wx_domains or wt_int > wx_domains[(w, p)][0]:
                wx_domains[(w, p)] = (wt_int, mapped_cat, mapped_src)
            wx_domain_by_w[w].append((p, wt_int, mapped_cat, mapped_src))

    def is_safe_entry(w: str, p: str) -> bool:
        if (w, p) in hot_keys:
            return False
        if normalize_phrase(w) != w:
            return False
        syllables = p.split("'")
        if len(syllables) != len(w) or PINYIN_RE.fullmatch(p) is None:
            return False
        if (w, p) in base_key_set:
            return True
        canon = p.replace("'", "")
        if canon in eng_words and canon not in allowed_english_homograph_pinyins:
            return False
        if canon in PROTECTED_CANON_PINYINS and (w, p) not in sc_keys:
            return False
        if abbreviation_for(p) in PROTECTED_ABBREVS:
            return False
        return True

    entries: dict[tuple[str, str], tuple[int, str, str, str]] = {}

    def put_entry(w: str, p: str, freq: int, cat: str, src: str) -> bool:
        if not is_safe_entry(w, p):
            return False
        abbr = abbreviation_for(p)
        key = (w, p)
        if key not in entries or freq > entries[key][0]:
            entries[key] = (freq, cat, abbr, src)
        return True

    # Step 0: Baseline THUOCL & china-idiom polyphonic repair & bounded normalization
    repaired_count = 0
    for w, old_p, old_freq, cat, abbr, src in base_rows:
        new_p = old_p
        if w in EXPLICIT_POLYPHONIC_FIXES:
            new_p = EXPLICIT_POLYPHONIC_FIXES[w]
        else:
            upstream_votes: Counter[str] = Counter()
            old_supported = (w, old_p) in sc_keys
            for tbl_by_w in (wx_jichu_by_w, ri_base_by_w, ri_ext_by_w, cedict_by_w, wx_duoyin_by_w, wx_domain_by_w):
                if w in tbl_by_w:
                    cand_pinyins = {item[0] for item in tbl_by_w[w]}
                    if old_p in cand_pinyins:
                        old_supported = True
                    else:
                        upstream_votes[tbl_by_w[w][0][0]] += 1
            if not old_supported and upstream_votes:
                best_p, votes = upstream_votes.most_common(1)[0]
                if votes >= 2 or (w in wx_duoyin_by_w and wx_duoyin_by_w[w][0][0] == best_p):
                    if len(best_p.split("'")) == len(w):
                        new_p = best_p

        if new_p != old_p:
            repaired_count += 1
            base_key_set.add((w, new_p))

        jb_wt = jieba_dict.get(w, (0, ""))[0]
        if cat in ("person", "place"):
            scaled_freq = min(300, max(20, int(round(20 + 45 * math.log10(1 + max(1, old_freq))))))
        elif cat == "idiom":
            scaled_freq = min(400, max(60, int(round(80 + 40 * math.log10(1 + max(1, jb_wt))))))
        else:
            scaled_freq = min(
                500,
                max(30, int(round(40 + 60 * math.log10(1 + max(1, old_freq)) + 30 * math.log10(1 + jb_wt)))),
            )
        put_entry(w, new_p, scaled_freq, cat, src)

    baseline_count = len(entries)

    # Batch 1: Core common vocabulary, curated desktop/Linux/embedded terms & polyphonic calibration
    for w, p, freq, cat, src in CURATED_SUPPLEMENTS:
        if w in EXPLICIT_POLYPHONIC_FIXES:
            p = EXPLICIT_POLYPHONIC_FIXES[w]
        scaled = min(300 if cat in ("person", "place") else 850, max(120, int(round(freq / 6))))
        put_entry(w, p, scaled, cat, src)

    for w, p, freq, cat, src in REQUIRED_ACCEPTANCE_ENTRIES:
        base_key_set.add((w, p))
        put_entry(w, p, freq, cat, src)

    for w, p in EXPLICIT_POLYPHONIC_FIXES.items():
        if (w, p) not in entries:
            base_key_set.add((w, p))
            put_entry(w, p, 650, "daily", "wanxiang-duoyin")

    with (bundle / "KEY_PHRASE_READINGS.tsv").open(encoding="utf-8") as f:
        next(f)
        for line in f:
            w, p, wt, src, _ = line.rstrip("\n").split("\t")
            base_key_set.add((w, p))
            put_entry(w, p, 850, "daily", "wanxiang-jichu" if "wanxiang" in src else "rime-ice-base")

    for (w, p), (wt, _, _) in wx_duoyin.items():
        if wt >= 20 and (w in sc_words or (w, p) in ri_base or (w, p) in cedict or (w, p) in wx_jichu):
            ri_wt = ri_base.get((w, p), (0, "", ""))[0]
            freq = min(950, max(200, int(round(200 + 100 * math.log10(1 + wt) + 50 * math.log10(1 + ri_wt)))))
            put_entry(w, p, freq, "daily", "wanxiang-duoyin")

    for (w, p), (wx_wt, _, _) in wx_jichu.items():
        if (w, p) in sc_keys:
            continue
        ri_b = ri_base.get((w, p))
        ri_e = ri_ext.get((w, p))
        ce = cedict.get((w, p))
        jb = jieba_dict.get(w)
        jb_wt = jb[0] if jb else 0
        ri_wt = ri_b[0] if ri_b else (ri_e[0] if ri_e else 0)

        admit = False
        src_tag = "wanxiang-jichu"
        if ri_b is not None:
            if len(w) == 2 and (ri_b[0] >= 500 or (wx_wt >= 800 and jb_wt >= 20)):
                admit = True
            elif len(w) >= 3 and (ri_b[0] >= 200 or wx_wt >= 500):
                admit = True
        elif ri_e is not None and 3 <= len(w) <= 8:
            if wx_wt >= 600 and ri_e[0] >= 200:
                admit = True
                src_tag = "rime-ice-ext"
        elif ce is not None:
            if wx_wt >= 300 or jb_wt >= 30:
                admit = True
                src_tag = "cc-cedict"

        if admit:
            sources_hit = int(ri_b is not None) + int(ri_e is not None) + int(ce is not None) + int(jb is not None)
            consensus_bonus = 50 if sources_hit >= 2 else 0
            freq = min(
                1000,
                max(
                    150,
                    int(
                        round(
                            120
                            + 80 * math.log10(1 + wx_wt)
                            + 60 * math.log10(1 + ri_wt)
                            + 40 * math.log10(1 + jb_wt)
                            + consensus_bonus
                        )
                    ),
                ),
            )
            put_entry(w, p, freq, "daily", src_tag)

    after_b1_count = len(entries)
    batch1_added = after_b1_count - baseline_count

    # Batch 2: Jieba frequency grading + Wanxiang domain tables (person/place capped at 300)
    for w, (jb_wt, pos) in jieba_dict.items():
        if jb_wt < 80 or not (2 <= len(w) <= 8):
            continue
        if pos in ("nr", "ns") and jb_wt < 300:
            continue
        wx_cands = wx_jichu_by_w.get(w, [])
        ri_cands = ri_base_by_w.get(w, [])
        best_p = None
        wx_wt = 0
        if wx_cands and ri_cands and wx_cands[0][0] == ri_cands[0][0]:
            best_p = wx_cands[0][0]
            wx_wt = wx_cands[0][1]
        elif wx_cands and wx_cands[0][1] >= 400 and not any(c in STRONG_POLYPHONIC_CHARS for c in w):
            best_p = wx_cands[0][0]
            wx_wt = wx_cands[0][1]
        if best_p and (w, best_p) not in sc_keys and (w, best_p) not in entries:
            cat = "person" if pos == "nr" else ("place" if pos == "ns" else "daily")
            cap = 300 if cat in ("person", "place") else 500
            freq = min(cap, max(40, int(round(50 + 55 * math.log10(1 + jb_wt) + 40 * math.log10(1 + wx_wt)))))
            put_entry(w, best_p, freq, cat, "jieba")

    for (w, p), (dom_wt, mapped_cat, mapped_src) in wx_domains.items():
        if (w, p) in sc_keys or (w, p) in entries or not (2 <= len(w) <= 8):
            continue
        in_ri = (w, p) in ri_base or (w, p) in ri_ext
        in_ce = (w, p) in cedict
        jb_wt = jieba_dict.get(w, (0, ""))[0]
        if mapped_cat in ("person", "place"):
            if dom_wt >= 400 or in_ri or in_ce or jb_wt >= 80:
                freq = min(300, max(20, int(round(25 + 50 * math.log10(1 + dom_wt) + 30 * math.log10(1 + jb_wt)))))
                put_entry(w, p, freq, mapped_cat, mapped_src)
        else:
            if dom_wt >= 280 or in_ri or in_ce or jb_wt >= 30:
                freq = min(500, max(35, int(round(45 + 65 * math.log10(1 + dom_wt) + 35 * math.log10(1 + jb_wt)))))
                put_entry(w, p, freq, mapped_cat, mapped_src)

    after_b2_count = len(entries)
    batch2_added = after_b2_count - after_b1_count

    # Batch 3: Pending unannotated tables (tencent.dict.pending.tsv) + Polyphonic Quarantine
    quarantined: list[tuple[str, str, str, str, str, str, str, str, str, str]] = []
    quarantined_total_count = 0
    with (bundle / "normalized/rime-ice/tencent.dict.pending.tsv").open(encoding="utf-8") as f:
        next(f)
        for line in f:
            w, wt, _, _, _, line_no = line.rstrip("\n").split("\t")
            if w in sc_words or not (3 <= len(w) <= 6):
                continue
            poly_chars = "".join(sorted(set(c for c in w if c in STRONG_POLYPHONIC_CHARS)))
            wx_cands = wx_jichu_by_w.get(w, [])
            ri_cands = ri_base_by_w.get(w, []) or ri_ext_by_w.get(w, [])
            ce_cands = cedict_by_w.get(w, [])
            jb_wt = jieba_dict.get(w, (0, ""))[0]

            wx_p = wx_cands[0][0] if wx_cands else ""
            ri_p = ri_cands[0][0] if ri_cands else ""
            ce_p = ce_cands[0][0] if ce_cands else ""

            distinct_p = {p for p in (wx_p, ri_p, ce_p) if p}
            if poly_chars:
                agree_count = sum(1 for p in (wx_p, ri_p, ce_p) if p)
                if len(distinct_p) == 1 and agree_count >= 2 and wx_cands and wx_cands[0][1] >= 500:
                    best_p = next(iter(distinct_p))
                    if (w, best_p) not in entries:
                        freq = min(450, max(50, int(round(60 + 55 * math.log10(1 + wx_cands[0][1]) + 40 * math.log10(1 + jb_wt)))))
                        put_entry(w, best_p, freq, "daily", "rime-ice-tencent")
                elif len(distinct_p) > 1 or (wx_cands and wx_cands[0][1] >= 800 and agree_count == 1) or (jb_wt >= 150 and len(distinct_p) == 0):
                    quarantined_total_count += 1
                    if len(quarantined) < 400:
                        if len(distinct_p) > 1:
                            reason = "multi_source_reading_conflict"
                        elif agree_count == 1:
                            reason = "single_source_polyphonic_phrase_unconfirmed"
                        else:
                            reason = "polyphonic_without_annotated_phrase_reading"
                        quarantined.append((
                            w,
                            "quarantined",
                            ";".join(sorted(distinct_p)) if distinct_p else "UNANNOTATED",
                            poly_chars,
                            "rime-ice/tencent.dict.pending.tsv" + (";jieba/dict.pending.tsv" if jb_wt > 0 else ""),
                            str(max(100, jb_wt)),
                            wx_p or "-",
                            ri_p or "-",
                            ce_p or "-",
                            reason,
                        ))
            else:
                # Monophonic 3-6 char phrase corroborated by both rime-ice/tencent and wanxiang/jichu (wt >= 1000)
                if wx_p and len(wx_cands) == 1 and wx_cands[0][1] >= 1000 and (w, wx_p) not in entries:
                    freq = min(450, max(50, int(round(60 + 55 * math.log10(1 + wx_cands[0][1]) + 40 * math.log10(1 + jb_wt)))))
                    put_entry(w, wx_p, freq, "daily", "rime-ice-tencent")

    after_b3_count = len(entries)
    batch3_added = after_b3_count - after_b2_count

    sorted_rows = sorted(
        ((w, p, freq, cat, abbr, src) for (w, p), (freq, cat, abbr, src) in entries.items()),
        key=lambda r: (r[1], r[0], r[5]),
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.raw_output.parent.mkdir(parents=True, exist_ok=True)
    args.polyphonic_review_output.parent.mkdir(parents=True, exist_ok=True)

    with args.output.open("w", encoding="utf-8", newline="\n") as f_tsv, args.raw_output.open("w", encoding="utf-8", newline="\n") as f_raw:
        f_tsv.write("# ModernIME pinyin knowledge v1\n")
        f_tsv.write("# phrase<TAB>full_pinyin<TAB>frequency<TAB>category<TAB>abbreviation<TAB>source\n")
        for w, p, freq, cat, abbr, src in sorted_rows:
            f_tsv.write(f"{w}\t{p}\t{freq}\t{cat}\t{abbr}\t{src}\n")
            f_raw.write(f"{w}\t{p}\t0\n")

    with args.polyphonic_review_output.open("w", encoding="utf-8", newline="\n") as f_rev:
        f_rev.write("# ModernIME polyphonic quarantine manifest v1\n")
        f_rev.write(
            "# phrase<TAB>status<TAB>candidate_pinyins<TAB>polyphonic_chars<TAB>pending_sources"
            "<TAB>pending_weight<TAB>wanxiang_pinyin<TAB>rime_ice_pinyin<TAB>cedict_pinyin<TAB>reason\n"
        )
        for row in sorted(quarantined, key=lambda r: (r[9], r[0])):
            f_rev.write("\t".join(row) + "\n")

    distinct_words = len({w for w, _ in entries})
    combined_words = len(sc_words | hot_words | {w for w, _ in entries})
    combined_keys = len(sc_keys | hot_keys | set(entries.keys()))

    by_source = Counter(src for _, (_, _, _, src) in entries.items())
    by_category = Counter(cat for _, (_, cat, _, _) in entries.items())

    summary = {
        "total_entries": len(sorted_rows),
        "distinct_phrases": distinct_words,
        "polyphonic_extra_readings": len(sorted_rows) - distinct_words,
        "baseline_entries": baseline_count,
        "baseline_polyphonic_repairs": repaired_count,
        "batch1_added": batch1_added,
        "batch2_added": batch2_added,
        "batch3_added": batch3_added,
        "batch3_quarantined_total": quarantined_total_count,
        "batch3_quarantined_manifest_rows": len(quarantined),
        "combined_system_distinct_phrases": combined_words,
        "combined_system_distinct_keys": combined_keys,
        "by_source": dict(sorted(by_source.items())),
        "by_category": dict(sorted(by_category.items())),
        "sha256": {
            "modernime-knowledge.tsv": sha256_file(args.output),
            "modernime-knowledge.raw": sha256_file(args.raw_output),
            "POLYPHONIC_REVIEW.tsv": sha256_file(args.polyphonic_review_output),
        },
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
