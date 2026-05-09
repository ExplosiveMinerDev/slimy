#include "game/MapIO.h"
#include "game/Slime.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pe {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

bool parseTagToken(const std::string& tok, int& outTag, std::string* errOut) {
    if (tok == "grass") {
        outTag = Slime::grassTag;
        return true;
    }
    if (tok == "stone") {
        outTag = Slime::stoneTag;
        return true;
    }
    if (tok == "platform") {
        outTag = Slime::platformTag;
        return true;
    }
    if (tok == "redrock") {
        outTag = Slime::mapTestRockTag;
        return true;
    }
    if (tok == "vent") {
        outTag = Slime::airVentTag;
        return true;
    }
    try {
        outTag = std::stoi(tok);
        return true;
    } catch (...) {
        if (errOut) *errOut = "bad tag: " + tok;
        return false;
    }
}

const char* tagNameForSave(int tag) {
    if (tag == Slime::grassTag) return "grass";
    if (tag == Slime::stoneTag) return "stone";
    if (tag == Slime::platformTag) return "platform";
    if (tag == Slime::mapTestRockTag) return "redrock";
    if (tag == Slime::airVentTag) return "vent";
    return "platform";
}

} // namespace

bool parseSolidMapFile(const std::string& path, std::vector<SolidMapEntry>& out, std::string* errOut) {
    out.clear();
    std::ifstream f(path);
    if (!f.good()) {
        if (errOut) *errOut = "cannot open file";
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd != "box") {
            if (errOut)
                *errOut = "line " + std::to_string(lineNo) + ": expected 'box'";
            return false;
        }
        float cx = 0.f, cy = 0.f, hw = 0.f, hh = 0.f;
        std::string tagTok;
        if (!(iss >> cx >> cy >> hw >> hh >> tagTok)) {
            if (errOut)
                *errOut = "line " + std::to_string(lineNo) + ": box cx cy halfW halfH tag";
            return false;
        }
        if (hw <= 0.f || hh <= 0.f) {
            if (errOut) *errOut = "line " + std::to_string(lineNo) + ": half extents must be > 0";
            return false;
        }
        int tag = Slime::platformTag;
        if (!parseTagToken(tagTok, tag, errOut)) return false;
        SolidMapEntry e;
        e.pos = {cx, cy};
        e.halfW = hw;
        e.halfH = hh;
        e.tag = tag;
        out.push_back(e);
    }
    return true;
}

bool writeSolidMapFile(const std::string& path, const std::vector<SolidMapEntry>& in, std::string* errOut) {
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.good()) {
        if (errOut) *errOut = "cannot write file";
        return false;
    }
    f << "# SlimyJourney solid map (.sjmap)\n";
    f << "# Lines:  box  centerX  centerY  halfW  halfH  tag\n";
    f << "# tag: grass | stone | platform | redrock | vent  (or integer)\n";
    f << std::fixed << std::setprecision(3);
    for (const auto& e : in) {
        f << "box " << e.pos.x << ' ' << e.pos.y << ' ' << e.halfW << ' ' << e.halfH << ' '
          << tagNameForSave(e.tag) << '\n';
    }
    return true;
}

} // namespace pe
