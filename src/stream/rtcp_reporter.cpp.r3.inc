ck(h);
        }
        hits.swap(uniq);
    }
    if (hits.empty()) return false;

    // Cap per feedback: in 01.35 count 222/314 → UDP ~MTU e "loss" enorme.
    // Max 40 hit (≥100 ms a ~400 pkt/s) resta sotto ~400 B con mtu 1200.
    constexpr int kMaxHits = 40;
    if (static_cast<int>(hits.size()) > kMaxHits)
        hits.erase(hits.begin(), hits.end() - kMaxHits);

    uint16_t base = hits.front().seq;
    uint16_t last = hits.back().seq;
    int statusCount = static_cast<int>(static_cast<uint16_t>(last - base)) + 1;
    // Range troppo largo rispetto ai pacchetti ricevuti = buco fittizio da
    // seq parse errato (ext id 2/4 in 01.35): il server spegne l'RTP.
    if (statusCount > static_cast<int>(hits.size()) * 4 || statusCount > 80) {
        TwccHit keep = hits.back();
        hits.clear();
        hits.push_back(keep);
        base = keep.seq;
        last = base;
        statusCount = 1;
    }
    if (statusCount > 0x1FFF) statusCount = 0x1FFF;
    if (statusCount <= 0) return false;

    std::vector<uint64_t> arr(static_cast<size_t>(statusCount), 0);
    for (const auto& h : hits) {
        int idx = static_cast<uint16_t>(h.seq - base);
        if (idx >= 0 && idx < statusCount) arr[static_cast<size_t>(idx)] = h.us;
    }

    // reference time: 24 bit × 64 ms, allineato al primo arrivo (delta≈0).
    uint64_t firstUs = hits.front().us;
    uint32_t refMs64 = static_cast<uint32_t>((firstUs / 1000u) / 64u) & 0x00FFFFFFu;
    uint64_t prevUs = static_cast<uint64_t>(refMs64) * 64000ull;

    std::vector<uint16_t> chunks;
    std::vector<uint8_t> deltas;
    int i = 0;
    while (i < statusCount) {
        bool recv = arr[static_cast<size_t>(i)] != 0;
        int run = 1;
        // Per i ricevuti non si usa un unico run se servono large delta misti:
        // status 1 = small (1 byte), 2 = large (2 byte). Si spezza per tipo.
        if (!recv) {
            while (i + run < statusCount && run < 0x1FFF && arr[static_cast<size_t>(i + run)] == 0) ++run;
 