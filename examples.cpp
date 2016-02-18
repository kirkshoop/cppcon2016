
// em++ -std=c++14 --memory-init-file 0 -s EXPORTED_FUNCTIONS="['_main', '_reset', '_rxlinesfrombytes', '_rxhttp', '_designpush', '_designoperator', '_designlast']" -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -O2 examples.cpp -o examples.js

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
    void designpush(int, int);
    void designoperator(int, int);
    void designlast(int, int);
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
    designlast(0, 10);
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

namespace designpushdef
{
/*
template<class V>
struct receiver
{
    void operator()(V v);
};

template<class SenderV>
struct algorithm
{
    SenderU operator()(SenderV s);
};

template<class Receiver>
struct sender
{
    void operator()(Receiver r);
};
*/

template<class Next>
struct receiver
{
    Next i;
    template<class V>
    void operator()(V&& v) const {
        return i(std::forward<V>(v));
    }
};
template<class Next>
auto make_receiver(Next i) {
    return receiver<std::decay_t<Next>>{i};
}
template<class T>
struct receiver_check : public false_type {};

template<class Next>
struct receiver_check<receiver<Next>> : public true_type {};

template<class T>
using for_receiver = typename enable_if<receiver_check<T>::value>::type;

template<class T>
using not_receiver = typename enable_if<!receiver_check<T>::value>::type;

const auto ints = [](auto first, auto last){
    return [=](auto r){
        for(auto i=first;i <= last; ++i){
            r(i);
        }
    };
};
const auto copy_if = [](auto pred){
    return [=](auto dest){
        return [=](auto v){
            if (pred(v)) dest(v);
        };
    };
};
const auto printto = [](auto& output){
    return make_receiver([&](auto v) {
        output << v << endl;
    });
};

template<class SenderV, class SenderU, class CheckS = not_receiver<SenderU>>
auto operator|(SenderV sv, SenderU su){
    return [=](auto dest){
        return sv(su(dest));
    };
}

template<class SenderV, class ReceiverV, class CheckS = not_receiver<SenderV>, class CheckR = for_receiver<ReceiverV>>
auto operator|(SenderV sv, ReceiverV rv) {
    return sv(rv);
}


}

auto even = [](auto v){return (v % 2) == 0;};

void designpush(int first, int last){
    using namespace designpushdef;
    ints(first, last)(designpushdef::copy_if(even)(printto(cout)));
}

void designoperator(int first, int last){
    using namespace designpushdef;
    ints(first, last) | designpushdef::copy_if(even) | printto(cout);
}

namespace designcontractdef 
{
/*
template<class V>
struct receiver
{
    void operator()(V v);
    void operator()(exception_ptr);
    void operator()();
};

template<class SenderV>
struct algorithm
{
    SenderU operator()(SenderV s);
};

template<class Receiver>
struct sender
{
    void operator()(Receiver r);
};
*/
auto report = [](auto&& e, auto&& f, auto&&... args){
    try{f(args...);} catch(...) {e(current_exception());}
};

auto enforce = [](bool& stopped, auto&& f) {
    return [&](auto&&... args){
        if (!stopped) f(args...);
    };
};

auto end = [](bool& stopped, auto&& f, auto&&... cap) {
    return [&](auto&&... args){
        if (!stopped) { 
            stopped = true; 
            f(cap..., args...); 
        }
    };
};

template<class Payload = void>
struct state
{
    Payload p;
};
template<>
struct state<void>
{
};

template<class Next, class Error, class Complete, class State, class Dest>
struct receiver;

template<class T>
struct receiver_check : public false_type {};

template<class Next, class Error, class Complete, class State, class Dest>
struct receiver_check<receiver<Next, Error, Complete, State, Dest>> : public true_type {};

template<class T>
using for_receiver = enable_if_t<receiver_check<std::decay_t<T>>::value>;

template<class T>
using not_receiver = enable_if_t<!receiver_check<std::decay_t<T>>::value>;

struct noop
{
    // next
    template<class V, class Check = not_receiver<V>, class unique = void>
    void operator()(V&&) const {
    }
    // complete
    inline void operator()() const {
    }
    // delegating next
    template<class Dest, class V, class CheckD = for_receiver<Dest>, class CheckV = not_receiver<V>>
    void operator()(Dest&& d, V&& v) const {
        d(std::forward<V>(v));
    }
    // delegating complete
    template<class Dest, class Check = for_receiver<Dest>>
    void operator()(Dest&& d) const {
        d();
    }
};
struct ignore
{
    inline void operator()(exception_ptr) const {
    }
    template<class Dest>
    void operator()(Dest&& d, exception_ptr ep) const {
        d(ep);
    }
};
struct fail
{
    inline void operator()(exception_ptr ep) const {
        cout << "abort! " << what(ep) << endl;
        std::abort();
    }
};

//stateless
template<class Next, class Error, class Complete>
struct receiver<Next, Error, Complete, state<>, void>
{
    Next n;
    Error e;
    Complete c;
    mutable bool stopped = false;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(stopped, e), enforce(stopped, n), std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(stopped, e), ep);
    }
    inline void operator()() const {
        report(fail{}, end(stopped, c));
    }
};
//stateful
template<class Next, class Error, class Complete, class Payload>
struct receiver<state<Payload>, Next, Error, Complete, void>
{
    mutable state<Payload> s;
    Next n;
    Error e;
    Complete c;
    mutable bool stopped = false;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(stopped, e), enforce(stopped, n), s.p, std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(stopped, e), s.p, ep);
    }
    inline void operator()() const {
        report(fail{}, end(stopped, c), s.p);
    }
};
// stateless delegating
template<class DNext, class DError, class DComplete, class DState, class DDest, class Next, class Error, class Complete>
struct receiver<receiver<DNext, DError, DComplete, DState, DDest>, Next, Error, Complete, state<>>
{
    receiver<DNext, DError, DComplete, DState, DDest> d;
    Next n;
    Error e;
    Complete c;
    mutable bool stopped = false;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(stopped, e, d), enforce(stopped, n), d, std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(stopped, e), d, ep);
    }
    inline void operator()() const {
        report(fail{}, end(stopped, c), d);
    }
};
// stateful delegating
template<class DNext, class DError, class DComplete, class DState, class DDest, class Next, class Error, class Complete, class Payload>
struct receiver<receiver<DNext, DError, DComplete, DState, DDest>, state<Payload>, Next, Error, Complete>
{
    receiver<DNext, DError, DComplete, DState, DDest> d;
    mutable state<Payload> s;
    Next n;
    Error e;
    Complete c;
    mutable bool stopped = false;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(stopped, e, d, s.p), enforce(stopped, n), d, s.p, std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(stopped, e), d, s.p, ep);
    }
    inline void operator()() const {
        report(fail{}, end(stopped, c), d, s.p);
    }
};


//stateless
template<class Next = noop, class Error = fail, class Complete = noop,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, state<>, void>{n, e, c};
}
//stateful
template<class Payload, class Next = noop, class Error = fail, class Complete = noop,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(state<Payload> s, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<state<Payload>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, void>{s, n, e, c};
}
// stateless delegating
template<class Dest, class Next = noop, class Error = ignore, class Complete = noop,
class CheckD = for_receiver<Dest>,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>,
class unique = void>
auto make_receiver(Dest d, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Dest>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, state<>>{d, n, e, c};
}
// stateful delegating
template<class Dest, class Payload, class Next = noop, class Error = ignore, class Complete = noop,
class CheckD = for_receiver<Dest>,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(Dest d, state<Payload> s, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Dest>, state<Payload>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>>{d, s, n, e, c};
}

const auto ints = [](auto first, auto last){
    return [=](auto r){
        for(auto i=first;i <= last; ++i){
            r(i);
        }
        r();
    };
};
const auto copy_if = [](auto pred){
    return [=](auto dest){
        return make_receiver(dest, [=](auto& d, auto v){
            if (pred(v)) d(v);
        });
    };
};
const auto last_or_default = [](auto def){
    return [=](auto dest){
        using store = state<std::decay_t<decltype(def)>>;
        return make_receiver(dest, store{def}, 
            [](auto& d, auto& s, auto v){
                s = v;
            },
            [](auto& d, auto& s, exception_ptr ep) {
                d(ep);
            },
            [](auto& d, auto& s){
                d(s);
                d();
            });
    };
};
const auto printto = [](auto& output){
    return make_receiver(
        [&](auto v) {
            output << v << endl;
        },
        [&](exception_ptr ep){
            output << what(ep) << endl;
        },
        [&](){
            output << "done!" << endl;
        });
};

template<class SenderV, class SenderU, class CheckS = not_receiver<SenderU>>
auto operator|(SenderV sv, SenderU su){
    return [=](auto dest){
        return sv(su(dest));
    };
}

template<class SenderV, class ReceiverV, class CheckS = not_receiver<SenderV>, class CheckR = for_receiver<ReceiverV>>
auto operator|(SenderV sv, ReceiverV rv) {
    return sv(rv);
}

}

void designlast(int first, int last){
    using namespace designcontractdef;
    ints(first, last) | designcontractdef::copy_if(even) | last_or_default(42) | printto(cout);
}
