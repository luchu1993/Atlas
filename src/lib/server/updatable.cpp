#include "server/updatable.hpp"

#include <algorithm>
#include <cassert>

namespace atlas
{

Updatables::Updatables(int num_levels)
{
    assert(num_levels >= 1);
    // level_offsets_ has num_levels+1 entries: offsets[i]..offsets[i+1] is level i's range.
    level_offsets_.resize(static_cast<std::size_t>(num_levels) + 1, 0);
}

auto Updatables::add(Updatable* object, int level) -> bool
{
    assert(object != nullptr);
    assert(level >= 0 && level < num_levels());

    if (object->removal_handle_ >= 0)
        return false;  // already registered

    if (in_update_)
    {
        // Buffer for after call() completes
        pending_add_.push_back({object, level});
        // Assign a sentinel so double-add is caught
        object->removal_handle_ = -2;
        ++size_;
        return true;
    }

    // Insert at the end of the level's segment by shifting subsequent levels.
    int insert_pos = level_offsets_[static_cast<std::size_t>(level) + 1];
    objects_.insert(objects_.begin() + insert_pos, object);

    // Update offsets for levels > this one.
    for (int l = level + 1; l <= num_levels(); ++l)
        ++level_offsets_[static_cast<std::size_t>(l)];

    object->removal_handle_ = insert_pos;

    // Fix up removal_handle_ for objects that shifted right.
    for (int i = insert_pos + 1; i < level_offsets_[num_levels()]; ++i)
    {
        if (objects_[static_cast<std::size_t>(i)] != nullptr)
            objects_[static_cast<std::size_t>(i)]->removal_handle_ = i;
    }

    ++size_;
    return true;
}

auto Updatables::remove(Updatable* object) -> bool
{
    assert(object != nullptr);

    if (object->removal_handle_ < 0)
        return false;  // not registered (or already pending-add sentinel)

    int idx = object->removal_handle_;
    assert(idx < static_cast<int>(objects_.size()));
    assert(objects_[static_cast<std::size_t>(idx)] == object);

    objects_[static_cast<std::size_t>(idx)] = nullptr;
    object->removal_handle_ = -1;

    assert(size_ > 0);
    --size_;

    if (in_update_)
    {
        ++null_count_;
    }
    else
    {
        compact();
    }

    return true;
}

void Updatables::call()
{
    assert(!in_update_);
    in_update_ = true;

    int total = level_offsets_[static_cast<std::size_t>(num_levels())];
    for (int i = 0; i < total; ++i)
    {
        if (objects_[static_cast<std::size_t>(i)] != nullptr)
            objects_[static_cast<std::size_t>(i)]->update();
    }

    in_update_ = false;

    if (null_count_ > 0)
    {
        compact();
        null_count_ = 0;
    }

    // Flush pending additions
    for (auto& pa : pending_add_)
    {
        if (pa.object->removal_handle_ == -2)
        {
            pa.object->removal_handle_ = -1;  // reset sentinel so add() works
            --size_;                          // add() will re-increment
            add(pa.object, pa.level);
        }
    }
    pending_add_.clear();
}

void Updatables::compact()
{
    // Rebuild objects_ and level_offsets_ without null slots.
    std::vector<Updatable*> new_objects;
    new_objects.reserve(size_);

    std::vector<int> new_offsets(level_offsets_.size(), 0);

    for (int l = 0; l < num_levels(); ++l)
    {
        int start = level_offsets_[static_cast<std::size_t>(l)];
        int end = level_offsets_[static_cast<std::size_t>(l) + 1];

        new_offsets[static_cast<std::size_t>(l)] = static_cast<int>(new_objects.size());
        for (int i = start; i < end; ++i)
        {
            if (objects_[static_cast<std::size_t>(i)] != nullptr)
            {
                objects_[static_cast<std::size_t>(i)]->removal_handle_ =
                    static_cast<int>(new_objects.size());
                new_objects.push_back(objects_[static_cast<std::size_t>(i)]);
            }
        }
    }
    new_offsets[static_cast<std::size_t>(num_levels())] = static_cast<int>(new_objects.size());

    objects_ = std::move(new_objects);
    level_offsets_ = std::move(new_offsets);
}

}  // namespace atlas
