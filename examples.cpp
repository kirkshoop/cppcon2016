
// em++ -std=c++14 --memory-init-file 0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_rxlinesfrombytes']" -s DEMANGLE_SUPPORT=1 -O2 examples.cpp -o examples.js
#include "emscripten.h"

#include "rxcpp/rx.hpp"
using namespace rxcpp;
using namespace rxcpp::schedulers;
using namespace rxcpp::sources;
using namespace rxcpp::util;

#include <regex>
#include <random>
#include <chrono>
using namespace std;
using namespace std::literals;

extern"C" {
    void reset();
    void rxlinesfrombytes(int, int, int);
}

//
// setup up a run_loop scheduler and register a tick 
// function to dispatch from requestAnimationFrame
//

run_loop rl;

auto jsthread = observe_on_run_loop(rl);

void tick(){
    if (!rl.empty() && rl.peek().when < rl.now()) {
        rl.dispatch();
    }
}

int main() {
    emscripten_set_main_loop(tick, -1, false);
    return 0;
}

composite_subscription lifetime;

void reset() {
    lifetime.unsubscribe();
    lifetime = composite_subscription();
}

//
// produce byte vectors of random length strings ending in \r
//

struct UniformRandomInt
{
    random_device rd;   // non-deterministic generator
    mt19937 gen;
    uniform_int_distribution<> dist;

    UniformRandomInt(int start, int stop)
        : gen(rd())
        , dist(start, stop)
    {
    }
    int operator()() {
        return dist(gen);
    }
};

observable<vector<uint8_t>> readAsyncBytes(int stepms, int count, int windowSize)
{
    auto lengthProducer = make_shared<UniformRandomInt>(4, 18);
    
    auto step = chrono::milliseconds(stepms);

    auto s = jsthread;

    // produce byte stream that contains lines of text
    auto bytes = range(0, count).
        map([lengthProducer](int i){ 
            auto& getlength = *lengthProducer;
            return from((uint8_t)('A' + i)).
                repeat(getlength()).
                concat(from((uint8_t)'\r'));
        }).
        merge().
        window(windowSize).
        map([](observable<uint8_t> w){ 
            return w.
                reduce(
                    vector<uint8_t>(), 
                    [](vector<uint8_t>& v, uint8_t b){
                        v.push_back(b); 
                        return move(v);
                    }, 
                    [](vector<uint8_t>& v){return move(v);}).
                as_dynamic(); 
        }).
        merge();

    return interval(s.now() + step, step, s).
        zip([](int, vector<uint8_t> v){return v;}, bytes);
}

void rxlinesfrombytes(int stepms, int count, int windowSize)
{
    // create strings split on \r
    auto strings = readAsyncBytes(stepms, count, windowSize).
        tap([](vector<uint8_t>& v){
            copy(v.begin(), v.end(), ostream_iterator<long>(cout, " "));
            cout << endl; 
        }).
        map([](vector<uint8_t> v){
            string s(v.begin(), v.end());
            regex delim(R"/(\r)/");
            sregex_token_iterator cursor(s.begin(), s.end(), delim, {-1, 0});
            sregex_token_iterator end;
            vector<string> splits(cursor, end);
            return iterate(move(splits));
        }).
        concat();

    // group strings by line
    int group = 0;
    auto linewindows = strings.
        group_by(
            [=](string& s) mutable {
                return s.back() == '\r' ? group++ : group;
            },
            [](string& s) { return move(s);});

    // reduce the strings for a line into one string
    auto lines = linewindows.
        map([](grouped_observable<int, string> w){ 
            return w.
                take_until(w.filter([](string& s) mutable {
                    return s.back() == '\r';
                })).
                sum(); 
        }).
        merge();

    // print result
    lines.
        subscribe(
            lifetime,
            println(cout), 
            [](exception_ptr ep){cout << what(ep) << endl;});
}
