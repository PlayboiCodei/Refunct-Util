#include "refunct/custom_map_format.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv) {
    using namespace refunct::custom_maps;
    if(argc<2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(f)), {});
    Map m;
    std::string error;
    if(!Decode(json,m,error)){std::cerr<<error<<'\n';return 1;}
    assert(m.clusters[0].elements[0].size()==7);
    assert(m.clusters[30].elements[0].size()==5);
    for(const auto& [input,why] : std::initializer_list<std::pair<std::string,const char*>>{
        {"{}", "version"}, {"[]", "version"},
        {"{\"version\":2,\"clusters\":[]}","version"},
        {"{\"version\":1,\"clusters\":[]}","31"},
        {"{\"version\":1,\"clusters\":[],\"version\":1}", "duplicate"},
        {"{\"version\":1,\"clusters\":[],\"x\":NaN}","invalid"},
    }) {
        Map unused; std::string message;
        assert(!Decode(input, unused, message));
        if(message.find(why)==std::string::npos){std::cerr<<message<<'\n';return 3;}
    }
    std::cout<<"Custom map schema: valid sample; malformed inputs rejected\n";
}
