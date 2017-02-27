/*
* File:   lrucache.hpp
* Author: Alexander Ponomarev
*
* Created on June 20, 2013, 5:09 PM
*/

/*
Copyright (c) 2014, lamerman
All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
* Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
* Neither the name of lamerman nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _LRUCACHE_HPP_INCLUDED_
#define	_LRUCACHE_HPP_INCLUDED_

#include <unordered_map>
#include <list>
#include <cstddef>
#include <stdexcept>

namespace cache {

    template<typename key_t, typename value_t>
    class lru_cache {
    public:
        typedef typename std::pair<key_t, value_t> key_value_pair_t;
        typedef typename std::list<key_value_pair_t>::iterator list_iterator_t;

        lru_cache(size_t max_size) :
            _max_size(max_size) {
        }

        void put(const key_t& key, const value_t& value) {
            auto it = _cache_items_map.find(key);
            if(it != _cache_items_map.end()) {
                _cache_items_list.erase(it->second);
                _cache_items_map.erase(it);
            }

            _cache_items_list.push_front(key_value_pair_t(key, value));
            _cache_items_map[key] = _cache_items_list.begin();

            if(_cache_items_map.size() > _max_size) {
                auto last = _cache_items_list.end();
                last--;
                _cache_items_map.erase(last->first);
                _cache_items_list.pop_back();
            }
        }

        value_t& get(const key_t& key) {
            auto it = _cache_items_map.find(key);
            if(it == _cache_items_map.end()) {
                throw std::range_error("There is no such key in cache");
            } else {
                _cache_items_list.splice(_cache_items_list.begin(), _cache_items_list, it->second);
                return it->second->second;
            }
        }

        void erase_if_exists(const key_t& key) {
            auto it = _cache_items_map.find(key);
            if (it != _cache_items_map.end()) {
                _cache_items_list.erase(it->second);
                _cache_items_map.erase(it);
            }
        }

        template<typename F>
        void erase_if(const F &f) {
            for (auto it = std::begin(_cache_items_map); it != std::end(_cache_items_map);) {
                if (f(it->second->second)) {
                    _cache_items_list.erase(it->second);
                    it = _cache_items_map.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void clear() {
            _cache_items_map.clear();
            _cache_items_list.clear();
        }

        bool exists(const key_t& key) const {
            return _cache_items_map.find(key) != _cache_items_map.end();
        }

        size_t size() const {
            return _cache_items_map.size();
        }

    private:
        std::list<key_value_pair_t> _cache_items_list;
        std::unordered_map<key_t, list_iterator_t> _cache_items_map;
        size_t _max_size;
    };

} // namespace lru

#endif	/* _LRUCACHE_HPP_INCLUDED_ */
