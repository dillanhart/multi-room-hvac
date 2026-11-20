#include "history.h"

void history_ring::add(const history_entry& entry) {
    slots[next] = entry;
    next = (next + 1) % slots.size();
    if (count < slots.size()) count++;
}

std::vector<history_entry> history_ring::entries_since(long long since) const {
    std::vector<history_entry> out;
    size_t oldest = (next + slots.size() - count) % slots.size();
    for (size_t i = 0; i < count; i++) {
        const history_entry& e = slots[(oldest + i) % slots.size()];
        if (e.time > since) out.push_back(e);
    }
    return out;
}
