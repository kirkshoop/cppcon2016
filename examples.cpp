
// source ~/source/emsdk_portable/emsdk_env.sh

// em++ -std=c++14 --memory-init-file 0 -s ASSERTIONS=2 -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_rxlinesfrombytes', '_rxmousedrags', '_rxhttp', '_designpush', '_designoperator', '_designlast', '_designtake', '_designerror']" -O2 examples.cpp -o examples.js

#include "emscripten.h"
#include <emscripten/html5.h>

#include "rxcpp/rx.hpp"
using namespace rxcpp;
using namespace rxcpp::schedulers;
using namespace rxcpp::subjects;
using namespace rxcpp::sources;
using namespace rxcpp::util;

#include <regex>
#include <random>
#include <chrono>
using namespace std;
using namespace std::literals;

auto even = [](auto v){return (v % 2) == 0;};

auto always_throw = [](auto... ){
    throw runtime_error("always throw!");
    return true;
};

struct destruction
{
    ~destruction(){
        cout << "destructed" << endl;
    }
};

#include "rxcommon.h"
#include "rxlinesfrombytes.h"
#include "rxmousedrags.h"
#include "rxhttp.h"
#include "designpush.h"
#include "designcontract.h"

int main() {
    emscripten_set_main_loop(tick, -1, false);
    return 0;
}

