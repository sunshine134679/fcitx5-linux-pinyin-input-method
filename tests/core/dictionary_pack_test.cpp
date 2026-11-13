#include "modernime/pinyin/dictionary_pack.h"
#include "modernime/pinyin/pinyin_candidate_provider.h"
#include "modernime/pinyin/pinyin_shared_resources.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <thread>
#include <chrono>
using namespace modernime::pinyin;
static void require(bool v, const char *m) { if(!v) {std::cerr<<m<<'\n'; std::exit(1);} }
int main() {
 const auto root=std::filesystem::temp_directory_path()/("modernime-pack-"+std::to_string(getpid()));
 std::filesystem::remove_all(root); std::filesystem::create_directories(root/"source");
 std::ofstream(root/"source/manifest.ini")<<"id=test-domain\nname=测式翎域\nsource=项目自编示例\nlicense=CC0-1.0\nversion=1\nentry_count=1\n";
 std::ofstream(root/"source/entries.tsv")<<"ce'shi'ling'yu\t测式翎域\t0\n";
 DictionaryPackStore store(root/"installed"); std::string error;
 require(store.validate(root/"source", &error).has_value(), "valid offline pack");
 require(store.importPack(root/"source",false,&error),"import pack");
 require(store.list().size()==1 && store.list()[0].enabled,"list enabled manifest");
 PinyinDataPaths paths; paths.dictionaryPacks=(root/"installed").string();paths.bundledDictionaryPacks=(root/"none").string(); paths.learningStore=(root/"learn.sqlite").string();
 PinyinProviderOptions opts; opts.learningEnabled=false;
 auto shared=PinyinCandidateProvider::createSharedResources(paths,opts);PinyinCandidateProvider p(shared,opts);
 p.replaceInput("ceshilingyu"); auto has=[&]{for(auto&i:p.page().items)if(i.text=="测式翎域")return true;return false;};require(has(),"enabled pack reaches provider");
 auto old=p.page().generation;require(shared->setDictionaryPackEnabled("test-domain",false,&error),"disable");require(!has()&&p.page().generation>old,"disable revokes old candidates");
 require(shared->setDictionaryPackEnabled("test-domain",true,&error)&&has(),"enable reload");
 require(store.setEnabled("test-domain",false,&error),"external worker disables package");
 shared->requestDictionaryPackRefresh();
 bool published=false;
 for(int attempt=0;attempt<400&&!published;++attempt) {
   published=shared->consumeDictionaryPackRefresh().changed;
   if(!published)std::this_thread::sleep_for(std::chrono::milliseconds(5));
 }
 require(published&&!has(),"prepared package reload replaces engine and revokes old page");
 require(shared->setDictionaryPackEnabled("test-domain",true,&error)&&has(),"enable after async refresh");
 std::ofstream(root/"source/manifest.ini")<<"id=test-domain\nname=测式翎域\nsource=项目自编示例\nlicense=CC0-1.0\nversion=2\nentry_count=1\n";
 std::ofstream(root/"source/entries.tsv")<<"ce'shi'ling'yu\t测式翎域新版\t0\n";
 require(shared->importDictionaryPack(root/"source",true,&error),"update");require(!has(),"update removed old identity");
 require(shared->removeDictionaryPack("test-domain",&error),"remove");require(store.list().empty(),"removed only pack");
 std::filesystem::remove_all(root);
}
