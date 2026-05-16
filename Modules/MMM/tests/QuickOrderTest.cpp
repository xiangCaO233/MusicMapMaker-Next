#include "mmm/beatmap/BeatMap.h"
#include "log/colorful-log.h"
#include <fstream>

int main() {
    auto bm = MMM::BeatMap::loadFromFile(
        "/home/xiang/Documents/coding/c_cpp/MusicMapMaker-Next/Modules/MMM/tests/data/"
        "DJ Grimoire - Astral Quantization (EternityHyper) [Quantization [SV]].osu");
    
    XINFO("Timing points: {}", bm.m_timings.size());
    
    // Print order around t=68734-68815
    int idx = 0;
    for (const auto& t : bm.m_timings) {
        if (t.m_timestamp >= 68730 && t.m_timestamp <= 68820) {
            const char* eff = (t.m_timingEffect == MMM::TimingEffect::BPM) ? "BPM" : "SCROLL";
            XINFO("  [{}] t={:.1f} {} bpm={:.1f} param={:.4f}", 
                  idx, t.m_timestamp, eff, t.m_bpm, t.m_timingEffectParameter);
        }
        idx++;
    }
    
    bm.sync();
    bm.saveToFile("/tmp/quick_test_order.mc");
    
    // Read back and print effects
    std::ifstream fs("/tmp/quick_test_order.mc");
    nlohmann::json j;
    fs >> j;
    
    int bpm10000_count = 0;
    for (const auto& t : j["time"]) {
        if (std::abs(t["bpm"].get<double>() - 10000.0) < 1) bpm10000_count++;
    }
    XINFO("BPM=10000 in saved time: {}", bpm10000_count);
    XINFO("Effects in saved file: {}", j["effect"].size());
    
    for (int i = 0; i < std::min(15, (int)j["effect"].size()); i++) {
        auto& e = j["effect"][i];
        auto beat = e["beat"];
        double b = beat[0].get<double>() + beat[1].get<double>() / beat[2].get<double>();
        XINFO("  eff[{}] beat={:.4f} {}", i, b, e.dump());
    }
    
    return 0;
}
