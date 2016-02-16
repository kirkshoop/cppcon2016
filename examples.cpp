
// em++ -std=c++14 --memory-init-file 0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_rxlinesfrombytes', '_rxhttp']" -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -O2 examples.cpp -o examples.js
#include "emscripten.h"

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

extern"C" {
    void reset();
    void rxlinesfrombytes(int, int, int);
    void rxhttp(const char*);
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

struct progress_t
{
    int bytesLoaded;
    int totalSize;
};
struct response_t 
{
    struct state_t : public enable_shared_from_this<state_t>
    {
        state_t(std::string url) 
            : url(url)
            , loadhub({})
            , progresshub(progress_t{0, 0}) 
            {}
        std::string url;
        behavior<vector<uint8_t>> loadhub;
        behavior<progress_t> progresshub;
    };
    shared_ptr<state_t> state;
    response_t(std::string url) 
        : state(make_shared<state_t>(url))
        {}
    string url() const {return state->url;}
    observable<progress_t> progress() const {
        return state->progresshub.get_observable();
    }
    observable<vector<uint8_t>> load() const {
        return state->loadhub.get_observable();
    }
    void abort() const {
        state->loadhub.get_subscriber().unsubscribe();
    }
};
struct http_status_exception : public exception
{
    http_status_exception(int code, const char* m) : code(code), message(m) {}
    int code;
    string message;
    const char* what() const noexcept {return message.c_str();}
    static http_status_exception from(exception_ptr ep) {
        try { rethrow_exception(ep); }
        catch(http_status_exception he) {
            return he;
        }
    }
};
observable<response_t> httpGet(const char* urlArg)
{
    std::string url = urlArg;
    return create<response_t>([=](subscriber<response_t> dest){
        auto response = response_t(url);
        
        int token = emscripten_async_wget2_data(
            response.url().c_str(),
            "GET",
            "",
            response.state.get(),
            true, // the buffer is freed when unload returns
            [](unsigned, void* vp, void* d, unsigned s){
                auto state = reinterpret_cast<response_t::state_t*>(vp);
                
                state->progresshub.get_subscriber().on_completed();

                auto begin = reinterpret_cast<uint8_t*>(d);
                vector<uint8_t> data{begin, begin + s};
                state->loadhub.get_subscriber().on_next(data);
                state->loadhub.get_subscriber().on_completed();
            },
            [](unsigned, void* vp, int code, const char* m){
                auto state = reinterpret_cast<response_t::state_t*>(vp);

                state->progresshub.get_subscriber().on_completed();

                on_exception(
                    [=](){throw http_status_exception(code, m); return 0;}, 
                    state->loadhub.get_subscriber());
            },
            [](unsigned, void* vp, int p, int t){
                auto state = reinterpret_cast<response_t::state_t*>(vp);

                state->progresshub.get_subscriber().on_next(progress_t{p, t});
            });

        response.state->loadhub.get_subscriber().add([token, response](){
            emscripten_async_wget2_abort(token);
        });
        
        dest.on_next(response);
        dest.on_completed();
        
    });
}

struct model {
    struct data {
        int size;
        string line;
    };
    std::map<string, data> store;
};
std::ostream& operator<< (std::ostream& out, const model& m) {
    for (auto i : m.store) {
        auto url = i.first;
        auto d = i.second;
        out << url << ", " << d.size;
        if (!d.line.empty()) {
            out << endl << d.line;
        }
    }
    return out;
}

void rxhttp(const char* url){
    httpGet(url).
        map([](response_t response){
            return response.
                progress().
                start_with(progress_t{0,0}).
                combine_latest(
                    [=](progress_t p, vector<uint8_t> d){
                        return make_tuple(response.url(), p, d);
                    },
                    response.load().start_with(vector<uint8_t>{})).
                scan(
                    model{}, 
                    [](model m, tuple<string, progress_t, vector<uint8_t>> u){
                        apply(u, [&](string url, progress_t p, vector<uint8_t> d) {
                            auto& data = m.store[url];
                            data.line.assign(d.begin(), find(d.begin(), d.end(), '\n'));
                            data.size = max(p.bytesLoaded, int(d.size()));
                        });
                        return m;
                    });
        }).
        merge().
        subscribe(
            lifetime,
            println(cout), 
            [](exception_ptr ep){cout << endl << "error: " << http_status_exception::from(ep).code << endl;});
}