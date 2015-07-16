#ifndef AUTOCHECK_CLASSIFIER_HPP
#define AUTOCHECK_CLASSIFIER_HPP

#include <unordered_map>
#include <sstream>

#include "function.hpp"
#include "apply.hpp"

namespace autocheck {

  template <typename... Args>
  class classifier {
    public:
      typedef typename predicate<Args...>::type           pred_t;
      typedef std::function<std::string (const Args&...)> tagger_t;

    private:
      pred_t                                  is_trivial;
      size_t                                  num_trivial;
      std::vector<tagger_t>                   taggers;
      std::unordered_map<std::string, size_t> tag_cloud;

    public:
      classifier() :
        is_trivial(never()), num_trivial(0), taggers(), tag_cloud() {}

      classifier& trivial(const pred_t& pred) {
        is_trivial = pred;
        return *this;
      }

      classifier& collect(const tagger_t& tagger) {
        taggers.push_back(tagger);
        return *this;
      }

      template <
        typename Func,
        typename Enable = typename std::enable_if<
          !std::is_convertible<
            typename std::result_of<Func(const Args&...)>::type,
            std::string
          >::value
        >::type
      >
      classifier& collect(const Func& func) {
        taggers.push_back(
            [=] (const Args&... args) -> std::string {
              std::ostringstream ss;
              ss << func(args...);
              return ss.str();
            });
        return *this;
      }

      classifier& classify(const pred_t& pred, const std::string& label) {
        taggers.push_back(
            [=] (const Args&... args) {
              return (pred(args...)) ? label : "";
            });
        return *this;
      }

      void check(const std::tuple<Args...>& args) {
        if (apply(is_trivial, args)) ++num_trivial;

        std::string tags;
        for (tagger_t& tagger : taggers) {
          std::string tag = apply(tagger, args);
          if (tag.empty()) continue;
          if (!tags.empty()) tags += ", ";
          tags += tag;
        }
        if (!tags.empty()) ++tag_cloud[tags];
      }

      size_t trivial() const { return num_trivial; }

      distribution distro() const {
        return distribution(tag_cloud.begin(), tag_cloud.end());
      }
  };

  template <typename Classifier>
  Classifier&& trivial( const typename Classifier::pred_t& pred,
      Classifier&& cls)
  {
    cls.trivial(pred);
    return std::forward<Classifier>(cls);
  }

  template <typename Classifier>
  Classifier&& collect(const typename Classifier::tagger_t& tagger,
      Classifier&& cls)
  {
    cls.collect(tagger);
    return std::forward<Classifier>(cls);
  }

  /* Missing lexical casting collect combinator, but no need until the
   * combinators can be used. */

  template <typename Classifier>
  Classifier&& classify(const typename Classifier::pred_t& pred,
      const std::string& label,
      Classifier&& cls)
  {
    cls.classify(pred, label);
    return std::forward<Classifier>(cls);
  }

}

#endif

