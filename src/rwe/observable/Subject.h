#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <rwe/observable/Observable.h>
#include <rwe/observable/Subscription.h>

namespace rwe
{
    template <typename T>
    class Subject : public Observable<T>
    {
    public:
        using SubscriberCallback = typename Observable<T>::SubscriberCallback;

    private:
        using SubscriberId = unsigned int;

        struct Subscriber
        {
            SubscriberId id;
            SubscriberCallback callback;
        };

        class ConcreteSubscription : public Subscription
        {
        private:
            Subject* observable;
            SubscriberId id;
            std::shared_ptr<std::atomic_bool> observableAlive;

        public:
            ConcreteSubscription(Subject<T>* observable, SubscriberId id, std::shared_ptr<std::atomic_bool> observableAlive)
                : observable(observable), id(id), observableAlive(std::move(observableAlive))
            {
            }

            ConcreteSubscription(const ConcreteSubscription&) = delete;
            ConcreteSubscription& operator=(const ConcreteSubscription&) = delete;

        protected:
            void unsubscribe() override
            {
                // the subject may already be destroyed: components can
                // own subscriptions to subjects that die first (e.g. a
                // UiListBox that stores a subscription to its own
                // selectedIndex subject, destroyed before ~UiComponent
                // unsubscribes)
                if (!observableAlive->load(std::memory_order_acquire))
                {
                    return;
                }
                observable->unsubscribe(id);
            }
        };

    private:
        SubscriberId nextId{0};

        /**
         * Guards `subscribers`. Publication is not main-thread-only:
         * the SDL mixer invokes AudioService's stopped-track callback
         * (and therefore channelFinished.next()) on the audio device
         * thread while scenes subscribe and unsubscribe on the main
         * thread. Holding the lock across delivery makes unsubscribe
         * wait for any in-flight callback to return, so a callback can
         * never observe a destroyed owner mid-call. The mutex is
         * recursive because callbacks re-enter next()/subscribe()/
         * unsubscribe() on the same thread (e.g. a selection callback
         * that re-populates a listbox).
         */
        std::recursive_mutex mutex;

        std::vector<Subscriber> subscribers;

        /**
         * Shared liveness flag handed to every Subscription. A
         * Subscription can outlive its Subject (see unsubscribe());
         * when the Subject dies the flag lets the stale handle become
         * a no-op instead of dereferencing a dead object.
         */
        std::shared_ptr<std::atomic_bool> alive{std::make_shared<std::atomic_bool>(true)};

    public:
        ~Subject()
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            alive->store(false, std::memory_order_release);
        }

        void next(const T& newValue)
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            // deliver on a snapshot so a callback that mutates the
            // subscriber list cannot invalidate this iteration
            auto snapshot = subscribers;
            for (const Subscriber& s : snapshot)
            {
                s.callback(newValue);
            }
        }

        std::unique_ptr<Subscription> subscribe(const SubscriberCallback& onNext) override
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            auto id = nextId++;
            subscribers.push_back({id, onNext});

            return std::unique_ptr<Subscription>(new ConcreteSubscription(this, id, alive));
        }

        std::unique_ptr<Subscription> subscribe(SubscriberCallback&& onNext) override
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            auto id = nextId++;
            subscribers.push_back({id, std::move(onNext)});

            return std::unique_ptr<Subscription>(new ConcreteSubscription(this, id, alive));
        }

    private:
        void unsubscribe(SubscriberId id)
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            auto it = std::find_if(subscribers.begin(), subscribers.end(), [id](const Subscriber& s) { return s.id == id; });
            if (it == subscribers.end())
            {
                return;
            }

            subscribers.erase(it);
        }
    };
}
