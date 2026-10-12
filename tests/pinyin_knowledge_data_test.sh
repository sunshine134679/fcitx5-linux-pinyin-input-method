#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 DATA_FILE RAW_FILE [HOTWORDS_FILE]" >&2
    exit 2
fi

data_file=$1
raw_file=$2
hotwords_file=${3:-$(dirname "$raw_file")/modernime-hotwords.raw}

if [[ ! -s "$data_file" ]]; then
    echo "拼音知识库数据文件不存在或为空：$data_file" >&2
    exit 1
fi
if [[ ! -s "$raw_file" ]]; then
    echo "LibIME raw 数据文件不存在或为空：$raw_file" >&2
    exit 1
fi

tmp_sorted=$(mktemp)
tmp_raw_sorted=$(mktemp)
cleanup() {
    rm -f -- "$tmp_sorted" "$tmp_raw_sorted"
}
trap cleanup EXIT

data_count=$(awk -F '\t' '
    /^#/ { next }
    NF != 6 { bad = 1; next }
    {
        gsub(/\r$/, "")
        if ($1 == "" || $2 !~ /^[a-z]+('\''[a-z]+)*$/ ||
            $3 !~ /^[0-9]+$/ || $4 == "" || $5 !~ /^[a-z]+$/ ||
            $6 == "") {
            bad = 1
        }
        count++
    }
    END {
        if (bad) exit 1
        print count + 0
    }
' "$data_file")

if (( data_count < 200000 )); then
    echo "拼音知识库条目过少：$data_count（要求至少 200000）" >&2
    exit 1
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
python3 "$script_dir/../tools/generate_pinyin_knowledge.py" --self-test

for category in daily idiom it medical law place person finance chemistry biology; do
    if ! awk -F '\t' -v wanted="$category" '$4 == wanted { found = 1 } END { exit !found }' \
        "$data_file"; then
        echo "拼音知识库缺少必需类别：$category" >&2
        exit 1
    fi
done

check_entry() {
    local phrase=$1
    local pinyin=$2
    local abbreviation=$3
    if ! awk -F '\t' -v wanted_phrase="$phrase" -v wanted_pinyin="$pinyin" \
        -v wanted_abbreviation="$abbreviation" \
        '$1 == wanted_phrase && $2 == wanted_pinyin && $5 == wanted_abbreviation { found = 1 }
         END { exit !found }' "$data_file"; then
        echo "拼音知识库缺少代表词条：$phrase / $pinyin / $abbreviation" >&2
        exit 1
    fi
}

check_entry "一心一意" "yi'xin'yi'yi" "yxyy"
check_entry "自然语言处理" "zi'ran'yu'yan'chu'li" "zryycl"
check_entry "量子计算" "liang'zi'ji'suan" "lzjs"
check_entry "阿司匹林" "a'si'pi'lin" "aspl"
check_entry "候选栏" "hou'xuan'lan" "hxl"
check_entry "剪贴板" "jian'tie'ban" "jtb"
check_entry "上下文" "shang'xia'wen" "sxw"
check_entry "设备树" "she'bei'shu" "sbs"
check_entry "驱动程序" "qu'dong'cheng'xu" "qdcx"
check_entry "交叉编译" "jiao'cha'bian'yi" "jcby"
check_entry "嵌入式" "qian'ru'shi" "qrs"
check_entry "重庆" "chong'qing" "cq"
check_entry "重阳" "chong'yang" "cy"
check_entry "银行" "yin'hang" "yh"
check_entry "音乐" "yin'yue" "yy"
check_entry "快乐" "kuai'le" "kl"
check_entry "绿色" "lv'se" "ls"
check_entry "女儿" "nv'er" "ne"
check_entry "略微" "lve'wei" "lw"
check_entry "弹出窗口" "tan'chu'chuang'kou" "tcck"
check_entry "多重继承" "duo'chong'ji'cheng" "dcjc"
check_entry "方法重载" "fang'fa'chong'zai" "ffcz"
check_entry "调优" "tiao'you" "ty"
check_entry "暴力解法" "bao'li'jie'fa" "bljf"
check_entry "变长参数" "bian'chang'can'shu" "bccs"
check_entry "椎间盘" "zhui'jian'pan" "zjp"
check_entry "伺服电机" "si'fu'dian'ji" "sfdj"
check_entry "单雄信" "shan'xiong'xin" "sxx"

review_file="$(dirname "$data_file")/POLYPHONIC_REVIEW.tsv"
if [[ -s "$review_file" ]]; then
    review_count=$(awk -F '\t' '
        /^#/ { next }
        NF != 10 || $2 != "quarantined" { bad = 1 }
        { count++ }
        END { if (bad) exit 1; print count + 0 }
    ' "$review_file")
    if (( review_count < 50 )); then
        echo "多音字待复核清单条目过少：$review_count" >&2
        exit 1
    fi
fi

LC_ALL=C sort -t $'\t' -k2,2 -k1,1 -k6,6 "$data_file" > "$tmp_sorted"
if ! cmp -s "$data_file" "$tmp_sorted"; then
    echo "拼音知识库没有按完整拼音、词语和来源稳定排序" >&2
    exit 1
fi

raw_count=$(awk -F '\t' '
    NF != 3 || $1 == "" || $2 !~ /^[a-z]+('\''[a-z]+)*$/ || $3 != "0" { bad = 1 }
    { count++ }
    END { if (bad) exit 1; print count + 0 }
' "$raw_file")
if (( raw_count != data_count )); then
    echo "规范化数据和 LibIME raw 数据条数不一致：$data_count != $raw_count" >&2
    exit 1
fi

# 网络热词表：可选第三参数显式指定；缺省探测 raw 同目录下的
# modernime-hotwords.raw，存在才校验。
if [[ -n "${3:-}" && ! -s "$hotwords_file" ]]; then
    echo "网络热词数据文件不存在或为空：$hotwords_file" >&2
    exit 1
fi

hotwords_count=0
if [[ -s "$hotwords_file" ]]; then
    hotwords_count=$(awk -F '\t' '
        NF != 3 || $1 == "" || $2 !~ /^[a-z]+('\''[a-z]+)*$/ || $3 != "5" { bad = 1 }
        { count++ }
        END { if (bad) exit 1; print count + 0 }
    ' "$hotwords_file")
    if (( hotwords_count < 10 )); then
        echo "网络热词条目过少：$hotwords_count（要求至少 10）" >&2
        exit 1
    fi

    check_hotword() {
        local phrase=$1
        local pinyin=$2
        if ! awk -F '\t' -v wanted_phrase="$phrase" \
            -v wanted_pinyin="$pinyin" \
            '$1 == wanted_phrase && $2 == wanted_pinyin { found = 1 }
             END { exit !found }' "$hotwords_file"; then
            echo "网络热词缺少代表词条：$phrase / $pinyin" >&2
            exit 1
        fi
    }
    check_hotword "永远的神" "yong'yuan'de'shen"
    check_hotword "显眼包" "xian'yan'bao"

    # 热词与知识库 raw 重复会让同一词条进入多层词典，产生重复候选。
    if ! awk -F '\t' '
        NR == FNR { seen[$1 SUBSEP $2] = 1; next }
        seen[$1 SUBSEP $2] { dup = 1 }
        END { exit dup ? 1 : 0 }
    ' "$raw_file" "$hotwords_file"; then
        echo "网络热词与知识库 raw 存在重复词条（会造成重复候选）" >&2
        exit 1
    fi
fi

echo "拼音知识库数据测试通过：$data_count 条，raw $raw_count 条，热词 $hotwords_count 条"
