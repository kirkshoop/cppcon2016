
// em++ -std=c++14 --memory-init-file 0 -s EXPORTED_FUNCTIONS="['_rxlinesfrombytes']" -s DEMANGLE_SUPPORT=1 -s NO_EXIT_RUNTIME=1 -O2 examples.cpp -o examples.js

#include "rxcpp/rx.hpp"
using namespace rxcpp;
using namespace rxcpp::sources;
using namespace rxcpp::util;

#include <regex>
#include <random>
#include <chrono>
using namespace std;
using namespace std::literals;

extern"C" {
    void rxlinesfrombytes(int, int, int);
}

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

    auto s = identity_current_thread();
    //auto s = synchronize_new_thread();

    // produce byte stream that contains lines of text
    auto bytes = interval(s.now(), chrono::milliseconds(stepms), s).
        take(count).
        map([lengthProducer](int i){ 
            auto& getlength = *lengthProducer;
            return from((uint8_t)('A' + --i)).
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

    return bytes;
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
            println(cout), 
            [](exception_ptr ep){cout << what(ep) << endl;});
}

int main() {
    cout << ">rxlinesfrombytes" << endl;
    rxlinesfrombytes(10, 4, 11);
    cout << "~rxlinesfrombytes" << endl;
    
    return 0;
}