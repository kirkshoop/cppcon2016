#pragma once

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

template<class Payload = void>
struct state
{
    explicit state(Payload* p) : p(p) {}
    Payload& get() {
        return *p;
    }
    const Payload& get() const {
        return *p;
    }
private:
    Payload* p;
};
template<>
struct state<void>
{
};

//
// \brief represents the scope of an async operation. Holds a set of nested lifetiimes.
//
struct subscription
{
    subscription() : store(make_shared<shared>()) {}
    bool is_stopped() const {
        return store->stopped;
    }
    void add(const subscription& s) {
        if (s == *this) {std::abort();}
        store->others.insert(s);
        if (store->stopped) stop();
    }
    void erase(const subscription& s) {
        if (s == *this) {std::abort();}
        store->others.erase(s);
    }
    template<class Payload, class... ArgN>
    state<Payload> make_state(ArgN... argn) {
        auto p = make_unique<Payload>(argn...);
        auto result = state<Payload>{p.get()};
        store->destructors.emplace_front(
            [d=p.release()]() mutable {
                auto p = d; 
                d = nullptr; 
                delete p;
            });
        return result;
    }
    void stop() const {
        store->stopped = true;
        {
            auto expired = std::move(store->others);
            for (auto& o : expired) {
                o.stop();
            }
        }
        {
            auto destructors = std::move(store->destructors);
            for (auto& d : destructors) {
                d();
            }
        }
    }
private:
    struct shared
    {
        shared() : stopped(false) {}
        bool stopped;
        set<subscription> others;
        deque<function<void()>> destructors;
    };
    shared_ptr<shared> store;
    friend bool operator==(const subscription&, const subscription&);
    friend bool operator<(const subscription&, const subscription&);
};
bool operator==(const subscription& lhs, const subscription& rhs) {
    return lhs.store == rhs.store;
}
bool operator!=(const subscription& lhs, const subscription& rhs) {
    return !(lhs == rhs);
}
bool operator<(const subscription& lhs, const subscription& rhs) {
    return lhs.store < rhs.store;
}

auto report = [](auto&& e, auto&& f, auto&&... args){
    try{f(args...);} catch(...) {e(current_exception());}
};

auto enforce = [](const subscription& lifetime, auto&& f) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) f(args...);
    };
};

auto end = [](const subscription& lifetime, auto&& f, auto&&... cap) {
    return [&](auto&&... args){
        if (!lifetime.is_stopped()) { 
            f(cap..., args...); 
            lifetime.stop();
        }
    };
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

template<class T>
struct subscription_check : public false_type {};

template<>
struct subscription_check<subscription> : public true_type {};

template<class T>
using for_subscription = enable_if_t<subscription_check<std::decay_t<T>>::value>;

template<class T>
using not_subscription = enable_if_t<!subscription_check<std::decay_t<T>>::value>;

struct noop
{
    // next
    template<class V, class CheckR = not_receiver<V>, class CheckS = not_subscription<V>, class unique = void>
    void operator()(V&&) const {
    }
    // complete
    inline void operator()() const {
    }
    // lifetime next
    template<class V, class Check = not_receiver<V>>
    void operator()(const subscription& s, V&&) const {
    }
    // lifetime complete
    inline void operator()(const subscription& s) const {
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
    inline void operator()(const subscription&, exception_ptr) const {
    }
    template<class Dest, class CheckD = for_receiver<Dest>>
    void operator()(Dest&& d, exception_ptr ep) const {
        d(ep);
    }
    template<class Dest, class Payload, 
        class CheckD = for_receiver<Dest>, 
        class CheckP = not_receiver<Payload>>
    void operator()(Dest&& d, Payload&, exception_ptr ep) const {
        d(ep);
    }
};
struct fail
{
    template<class Payload, class CheckP = not_receiver<Payload>>
    void operator()(Payload&, exception_ptr ep) const {
        cout << "abort! " << what(ep) << endl << flush;
        std::abort();
    }
    inline void operator()(const subscription&, exception_ptr ep) const {
        cout << "abort! " << what(ep) << endl << flush;
        std::abort();
    }
    inline void operator()(exception_ptr ep) const {
        cout << "abort! " << what(ep) << endl << flush;
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
    subscription lifetime;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(lifetime, e, lifetime), enforce(lifetime, n), lifetime, std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(lifetime, e), lifetime, ep);
    }
    inline void operator()() const {
        report(fail{}, end(lifetime, c), lifetime);
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
    subscription lifetime;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(lifetime, e, lifetime, s.get()), enforce(lifetime, n), lifetime, s.get(), std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(lifetime, e), lifetime, s.get(), ep);
    }
    inline void operator()() const {
        report(fail{}, end(lifetime, c), lifetime, s.get());
    }
};
// stateless delegating
template<class DNext, class DError, class DComplete, class DState, class DDest, class Next, class Error, class Complete>
struct receiver<receiver<DNext, DError, DComplete, DState, DDest>, Next, Error, Complete, state<>>
{
    using Dest = receiver<DNext, DError, DComplete, DState, DDest>;
    Dest d;
    Next n;
    Error e;
    Complete c;
    subscription lifetime;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(lifetime, e, d), enforce(lifetime, n), d, std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(lifetime, e), d, ep);
    }
    inline void operator()() const {
        report(fail{}, end(lifetime, c), d);
    }
};
// stateful delegating
template<class DNext, class DError, class DComplete, class DState, class DDest, class Next, class Error, class Complete, class Payload>
struct receiver<receiver<DNext, DError, DComplete, DState, DDest>, state<Payload>, Next, Error, Complete>
{
    using Dest = receiver<DNext, DError, DComplete, DState, DDest>;
    Dest d;
    mutable state<Payload> s;
    Next n;
    Error e;
    Complete c;
    subscription lifetime;
    template<class V, class Check = enable_if_t<!is_same<std::decay_t<V>, exception_ptr>::value>>
    void operator()(V&& v) const {
        report(end(lifetime, e, d, s.get()), enforce(lifetime, n), d, s.get(), std::forward<V>(v));
    }
    inline void operator()(exception_ptr ep) const {
        report(fail{}, end(lifetime, e), d, s.get(), ep);
    }
    inline void operator()() const {
        report(fail{}, end(lifetime, c), d, s.get());
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
template<class Next = noop, class Error = fail, class Complete = noop,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(subscription l, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, state<>, void>{n, e, c, l};
}
//stateful
template<class Payload, class Next = noop, class Error = fail, class Complete = noop,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(state<Payload> s, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<state<Payload>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, void>{s, n, e, c};
}
template<class Payload, class Next = noop, class Error = fail, class Complete = noop,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(subscription l, state<Payload> s, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<state<Payload>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, void>{s, n, e, c, l};
}
// stateless delegating
template<class Dest, class Next = noop, class Error = ignore, class Complete = noop,
class CheckD = for_receiver<Dest>,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>,
class unique = void>
auto make_receiver(Dest d, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Dest>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>, state<>>{d, n, e, c, d.lifetime};
}
// stateful delegating
template<class Dest, class Payload, class Next = noop, class Error = ignore, class Complete = noop,
class CheckD = for_receiver<Dest>,
class CheckN = not_receiver<Next>,
class CheckE = not_receiver<Error>,
class CheckC = not_receiver<Complete>>
auto make_receiver(Dest d, state<Payload> s, Next n = Next{}, Error e = Error{}, Complete c = Complete{}) {
    return receiver<std::decay_t<Dest>, state<Payload>, std::decay_t<Next>, std::decay_t<Error>, std::decay_t<Complete>>{d, s, n, e, c, d.lifetime};
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
        auto last = dest.lifetime.template make_state<std::decay_t<decltype(def)>>(def);
        return make_receiver(dest, last, 
            [](auto& d, auto& l, auto v){
                l = v;
            },
            [](auto& d, auto& l, exception_ptr ep) {
                d(ep);
            },
            [](auto& d, auto& l){
                d(l);
                d();
            });
    };
};
const auto take = [](int n){
    return [=](auto dest){
        auto remaining = dest.lifetime.template make_state<int>(n);
        return make_receiver(dest, remaining, 
            [](auto& d, auto& r, auto v){
                if (r-- == 0) {
                    d();
                    return;
                }
                d(v);
            });
    };
};
const auto printto = [](auto& output){
    subscription lifetime;
    auto values = lifetime.template make_state<int>(0);
    return make_receiver(
        lifetime,
        values,
        [&](const subscription& l, auto& c, auto v) {
            ++c;
            output << v << endl;
        },
        [&](const subscription& l, auto& c, exception_ptr ep){
            output << what(ep) << endl;
        },
        [&](const subscription& l, auto& c){
            output << c << " values received - done!" << endl;
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

void designlast(int first, int last, int def){
    using namespace designcontractdef;
    ints(first, last) | designcontractdef::copy_if(even) | last_or_default(def) | printto(cout);
}

void designtake(int first, int last, int count){
    using namespace designcontractdef;
    ints(first, last) | designcontractdef::copy_if(even) | take(count) | printto(cout);
}
