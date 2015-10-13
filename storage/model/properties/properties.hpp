#ifndef MEMGRAPH_STORAGE_MODEL_PROPERTIES_PROPERTIES_HPP
#define MEMGRAPH_STORAGE_MODEL_PROPERTIES_PROPERTIES_HPP

#include <map>

#include "property.hpp"

class Properties
{
    using props_t = std::map<std::string, Property::sptr>;

public:
    props_t::iterator find(const std::string& key)
    {
        return props.find(key);
    }
    
    Property* at(const std::string& key)
    {
        auto it = props.find(key);
        return it == props.end() ? nullptr : it->second.get();
    }

    template <class T, class... Args>
    void emplace(const std::string& key, Args&&... args)
    {
        auto value = std::make_shared<T>(std::forward<Args>(args)...);

        // try to emplace the item
        auto result = props.emplace(std::make_pair(key, std::move(value)));

        // return if we succedded
        if(result.second)
            return;

        // the key already exists, replace the value it holds
        result.first->second = std::move(value);
    }

    void put(const std::string& key, Property::sptr value)
    {
        props[key] = std::move(value);
    }

    void clear(const std::string& key)
    {
        props.erase(key);
    }

    template <class Handler>
    void accept(Handler& handler)
    {
        bool first = true;

        for(auto& kv : props)
        {
            handler.handle(kv.first, *kv.second, first);
            
            if(first)
                first = false;
        }
    }

private:
    props_t props;
};

#endif
