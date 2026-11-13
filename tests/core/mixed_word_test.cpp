#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/core/learning_store.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace modernime::pinyin;
static void require(bool v,const char*m){if(!v){std::cerr<<m<<'\n';std::exit(1);}}
static std::size_t find(const modernime::core::CandidatePage&p,std::string_view text){for(std::size_t i=0;i<p.items.size();++i)if(p.items[i].text==text)return i;return p.items.size();}
int main(){
 auto dir=std::filesystem::temp_directory_path()/("modernime-mixed-"+std::to_string(getpid()));std::filesystem::create_directories(dir);
 PinyinDataPaths paths;paths.userDictionary=(dir/"user.tsv").string();paths.learningStore=(dir/"learn.sqlite").string();paths.dictionaryPacks=(dir/"packs").string();paths.bundledDictionaryPacks=MODERNIME_TEST_DICTIONARY_PACKS;
 PinyinProviderOptions options;
 {PinyinCandidateProvider p(paths,options);
 for(auto [input,text]:{std::pair{"agu","A股"},{"txu","T恤"},{"DockerRongQi","Docker容器"}}){require(p.replaceInput(input),"accept mixed input");auto i=find(p.page(),text);require(i<p.page().items.size(),"recall entire mixed word");require(p.page().items[i].consumedInputBytes==std::string_view(input).size(),"full raw consumption");require(p.select(i)&&p.page().rawInput.empty(),"selection consumes whole input");p.resetCommitHistory();}
 for(int n=0;n<3;++n){p.replaceInput("dockerrongqi");auto i=find(p.page(),"Docker容器");require(i<p.page().items.size()&&p.select(i),"real repeated mixed selection");p.resetCommitHistory();}
 require(p.flush(),"persist mixed learning");}
 paths.bundledDictionaryPacks=(dir/"none").string();
 {PinyinCandidateProvider p(paths,options);p.replaceInput("DOCKERRONGQI");auto i=find(p.page(),"Docker容器");require(i==0,"mature mixed word reaches global first after reopen");require(p.remove(i)&&p.flush(),"persist mixed suppression");}
 paths.bundledDictionaryPacks=MODERNIME_TEST_DICTIONARY_PACKS;
 options.learningEnabled=false;
 {PinyinCandidateProvider p(paths,options);p.replaceInput("dockerrongqi");require(find(p.page(),"Docker容器")==p.page().items.size(),"suppression respected with learning off");p.replaceInput("txu");auto i=find(p.page(),"T恤");require(i<p.page().items.size()&&p.select(i)&&p.flush(),"disabled mixed selection");}
 modernime::core::LearningStore store(paths.learningStore);require(store.open(),"open learning evidence");auto snapshot=store.snapshot();require(snapshot->entry("T恤","txu","","") != nullptr && snapshot->entry("T恤","txu","","")->frequency==1,"disabled choice did not increment");
 std::filesystem::remove_all(dir);
}
