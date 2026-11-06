// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// graph_export_formats.cpp -- graph-family AI-13: `graph export` to .png, .pdf
// and .eps written in the requested format at the requested path (w2ai
// gf_exp_069 `as(eps)`, gf_exp_004 `width(1600) height(1080)`, gf_exp_019 .pdf,
// gf_exp_asonly `as(png)` on a suffixless name, log:1922-2011). The source is
// the graph's own structural SVG (buildGraphSvg): its drawing elements are
// read into a display list and replayed as a PostScript program (EPS), a PDF
// content stream (Helvetica, no embedding) and a raster (PNG, stored-deflate
// zlib, no external library). Text is drawn in the vector formats; the raster
// draws the shapes only. JPEG (.jpg) is the same raster, baseline-encoded.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "piet/handlers.h"

namespace piet::handlers {

namespace {

struct Rgb {
  double r = 0, g = 0, b = 0;
  bool none = false;
};

Rgb parseColor(const std::string& s0) {
  Rgb c;
  std::string s = s0;
  s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
          s.end());
  if (s.empty() || s == "none" || s == "transparent") {
    c.none = true;
    return c;
  }
  auto hex = [](char h) -> int {
    if (h >= '0' && h <= '9') return h - '0';
    h = static_cast<char>(std::tolower(static_cast<unsigned char>(h)));
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    return 0;
  };
  if (s[0] == '#') {
    if (s.size() == 7) {
      c.r = (hex(s[1]) * 16 + hex(s[2])) / 255.0;
      c.g = (hex(s[3]) * 16 + hex(s[4])) / 255.0;
      c.b = (hex(s[5]) * 16 + hex(s[6])) / 255.0;
    } else if (s.size() == 4) {
      c.r = hex(s[1]) * 17 / 255.0;
      c.g = hex(s[2]) * 17 / 255.0;
      c.b = hex(s[3]) * 17 / 255.0;
    }
    return c;
  }
  if (s.compare(0, 4, "rgb(") == 0) {
    std::istringstream is(s.substr(4));
    char comma;
    int r = 0, g = 0, b = 0;
    is >> r >> comma >> g >> comma >> b;
    c.r = r / 255.0;
    c.g = g / 255.0;
    c.b = b / 255.0;
    return c;
  }
  static const std::map<std::string, std::array<int, 3>> kNamed = {
      {"black", {0, 0, 0}},      {"white", {255, 255, 255}}, {"red", {255, 0, 0}},
      {"green", {0, 128, 0}},    {"blue", {0, 0, 255}},      {"gray", {128, 128, 128}},
      {"grey", {128, 128, 128}}, {"navy", {0, 0, 128}},      {"orange", {255, 165, 0}},
      {"maroon", {128, 0, 0}},   {"lightgray", {211, 211, 211}}};
  auto it = kNamed.find(s);
  if (it != kNamed.end()) {
    c.r = it->second[0] / 255.0;
    c.g = it->second[1] / 255.0;
    c.b = it->second[2] / 255.0;
  }
  return c;
}

// An affine map (a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  void apply(double x, double y, double* ox, double* oy) const {
    *ox = a * x + c * y + e;
    *oy = b * x + d * y + f;
  }
  Affine then(const Affine& in) const {  // this ∘ in
    Affine r;
    r.a = a * in.a + c * in.b;
    r.b = b * in.a + d * in.b;
    r.c = a * in.c + c * in.d;
    r.d = b * in.c + d * in.d;
    r.e = a * in.e + c * in.f + e;
    r.f = b * in.e + d * in.f + f;
    return r;
  }
  double scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
};

struct Prim {
  enum Kind { kPath, kCircle, kText } kind = kPath;
  std::vector<std::vector<std::pair<double, double>>> subpaths;  // kPath (closed flags below)
  std::vector<bool> closed;
  double cx = 0, cy = 0, r = 0;  // kCircle
  Rgb fill, stroke;
  double width = 1;
  std::string text;  // kText
  double size = 10;
  std::string anchor = "start";
};

using Attrs = std::map<std::string, std::string>;

double num(const Attrs& a, const char* k, double dflt = 0) {
  auto it = a.find(k);
  if (it == a.end() || it->second.empty()) return dflt;
  return std::atof(it->second.c_str());
}
std::string str(const Attrs& a, const char* k, const std::string& dflt = std::string()) {
  auto it = a.find(k);
  return it == a.end() ? dflt : it->second;
}

Affine parseTransform(const std::string& t) {
  Affine m;
  size_t i = 0;
  while (i < t.size()) {
    const size_t open = t.find('(', i);
    if (open == std::string::npos) break;
    const size_t close = t.find(')', open);
    std::string name = t.substr(i, open - i);
    name.erase(std::remove_if(name.begin(), name.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
               name.end());
    std::string args = t.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
    for (char& ch : args)
      if (ch == ',') ch = ' ';
    std::istringstream is(args);
    std::vector<double> v;
    double x;
    while (is >> x) v.push_back(x);
    Affine step;
    if (name == "translate") {
      step.e = v.size() > 0 ? v[0] : 0;
      step.f = v.size() > 1 ? v[1] : 0;
    } else if (name == "scale") {
      step.a = v.size() > 0 ? v[0] : 1;
      step.d = v.size() > 1 ? v[1] : step.a;
    } else if (name == "rotate" && !v.empty()) {
      const double th = v[0] * M_PI / 180.0;
      step.a = std::cos(th);
      step.b = std::sin(th);
      step.c = -std::sin(th);
      step.d = std::cos(th);
    } else if (name == "matrix" && v.size() == 6) {
      step = Affine{v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    m = m.then(step);
    i = close == std::string::npos ? t.size() : close + 1;
  }
  return m;
}

std::string xmlUnescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      const size_t semi = s.find(';', i);
      if (semi != std::string::npos) {
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else out += '?';
        i = semi;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

struct Scene {
  double width = 520, height = 360;
  std::vector<Prim> prims;
};

// Reads the drawing elements of a structural SVG into a display list in the
// root viewBox's coordinates.
Scene parseSvg(const std::string& svg) {
  Scene sc;
  struct Frame {
    Affine m;
    Rgb fill;
    Rgb stroke;
    bool hasFill = false, hasStroke = false;
    double width = 1;
    bool hasWidth = false;
    std::string tag;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{});
  bool root = true;
  int skipDepth = 0;  // inside <clipPath>/<defs>
  size_t i = 0;
  while (i < svg.size()) {
    const size_t lt = svg.find('<', i);
    if (lt == std::string::npos) break;
    const size_t gt = svg.find('>', lt);
    if (gt == std::string::npos) break;
    std::string tagText = svg.substr(lt + 1, gt - lt - 1);
    i = gt + 1;
    if (tagText.empty() || tagText[0] == '!' || tagText[0] == '?') continue;
    if (tagText[0] == '/') {
      std::string name = tagText.substr(1);
      if (name == "clipPath" || name == "defs") {
        if (skipDepth > 0) --skipDepth;
        continue;
      }
      if ((name == "g" || name == "svg") && stack.size() > 1) stack.pop_back();
      continue;
    }
    const bool selfClose = !tagText.empty() && tagText.back() == '/';
    if (selfClose) tagText.pop_back();
    size_t ns = 0;
    while (ns < tagText.size() && !std::isspace(static_cast<unsigned char>(tagText[ns]))) ++ns;
    const std::string name = tagText.substr(0, ns);
    Attrs at;
    {
      size_t p = ns;
      while (p < tagText.size()) {
        while (p < tagText.size() && std::isspace(static_cast<unsigned char>(tagText[p]))) ++p;
        const size_t eq = tagText.find('=', p);
        if (eq == std::string::npos) break;
        std::string key = tagText.substr(p, eq - p);
        key.erase(std::remove_if(key.begin(), key.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }),
                  key.end());
        size_t q = eq + 1;
        while (q < tagText.size() && std::isspace(static_cast<unsigned char>(tagText[q]))) ++q;
        if (q >= tagText.size()) break;
        const char quote = tagText[q];
        if (quote != '"' && quote != '\'') break;
        const size_t end = tagText.find(quote, q + 1);
        at[key] = tagText.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
        p = end == std::string::npos ? tagText.size() : end + 1;
      }
      // style="a:b; c:d" as attributes
      auto st = at.find("style");
      if (st != at.end()) {
        std::istringstream is(st->second);
        std::string kv;
        while (std::getline(is, kv, ';')) {
          const size_t colon = kv.find(':');
          if (colon == std::string::npos) continue;
          std::string k = kv.substr(0, colon), v = kv.substr(colon + 1);
          k.erase(std::remove_if(k.begin(), k.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), k.end());
          while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
          at[k] = v;
        }
      }
    }
    if (name == "clipPath" || name == "defs") {
      if (!selfClose) ++skipDepth;
      continue;
    }
    if (skipDepth > 0) continue;
    Frame top = stack.back();
    auto inherit = [&](Frame& f) {
      if (at.count("fill")) {
        f.fill = parseColor(at["fill"]);
        f.hasFill = true;
      }
      if (at.count("stroke")) {
        f.stroke = parseColor(at["stroke"]);
        f.hasStroke = true;
      }
      if (at.count("stroke-width")) {
        f.width = std::atof(at["stroke-width"].c_str());
        f.hasWidth = true;
      }
    };
    if (name == "svg") {
      Frame f = top;
      std::istringstream vb(str(at, "viewBox"));
      double vx = 0, vy = 0, vw = 0, vh = 0;
      vb >> vx >> vy >> vw >> vh;
      if (root) {
        if (vw > 0 && vh > 0) {
          sc.width = vw;
          sc.height = vh;
        }
        Affine m;
        m.e = -vx;
        m.f = -vy;
        f.m = m;
        root = false;
      } else {
        const double x = num(at, "x"), y = num(at, "y");
        const double w = num(at, "width", vw), h = num(at, "height", vh);
        Affine m;
        const double sx = vw > 0 ? w / vw : 1, sy = vh > 0 ? h / vh : 1;
        m.a = sx;
        m.d = sy;
        m.e = x - vx * sx;
        m.f = y - vy * sy;
        f.m = top.m.then(m);
      }
      inherit(f);
      if (!selfClose) stack.push_back(f);
      continue;
    }
    if (name == "g") {
      Frame f = top;
      if (at.count("transform")) f.m = top.m.then(parseTransform(at["transform"]));
      inherit(f);
      if (!selfClose) stack.push_back(f);
      continue;
    }
    Frame f = top;
    if (at.count("transform")) f.m = top.m.then(parseTransform(at["transform"]));
    inherit(f);
    Prim pr;
    pr.fill = f.hasFill ? f.fill : Rgb{0, 0, 0, false};
    pr.stroke = f.hasStroke ? f.stroke : Rgb{0, 0, 0, true};
    pr.width = (f.hasWidth ? f.width : 1) * f.m.scale();
    auto pt = [&](double x, double y) {
      double ox, oy;
      f.m.apply(x, y, &ox, &oy);
      return std::make_pair(ox, oy);
    };
    if (name == "line") {
      pr.kind = Prim::kPath;
      pr.subpaths.push_back({pt(num(at, "x1"), num(at, "y1")), pt(num(at, "x2"), num(at, "y2"))});
      pr.closed.push_back(false);
      if (!f.hasStroke) pr.stroke = Rgb{0, 0, 0, false};
      pr.fill.none = true;
      sc.prims.push_back(pr);
    } else if (name == "rect") {
      const double x = num(at, "x"), y = num(at, "y"), w = num(at, "width"), h = num(at, "height");
      pr.kind = Prim::kPath;
      pr.subpaths.push_back({pt(x, y), pt(x + w, y), pt(x + w, y + h), pt(x, y + h)});
      pr.closed.push_back(true);
      sc.prims.push_back(pr);
    } else if (name == "circle") {
      pr.kind = Prim::kCircle;
      auto c = pt(num(at, "cx"), num(at, "cy"));
      pr.cx = c.first;
      pr.cy = c.second;
      pr.r = num(at, "r") * f.m.scale();
      sc.prims.push_back(pr);
    } else if (name == "polyline" || name == "polygon") {
      std::string pts = str(at, "points");
      for (char& ch : pts)
        if (ch == ',') ch = ' ';
      std::istringstream is(pts);
      std::vector<std::pair<double, double>> sp;
      double x, y;
      while (is >> x >> y) sp.push_back(pt(x, y));
      pr.kind = Prim::kPath;
      pr.subpaths.push_back(sp);
      pr.closed.push_back(name == "polygon");
      if (name == "polyline" && !f.hasFill) pr.fill.none = true;
      sc.prims.push_back(pr);
    } else if (name == "path") {
      std::string d = str(at, "d");
      std::vector<std::pair<double, double>> cur;
      double px = 0, py = 0;
      char cmd = 'M';
      size_t q = 0;
      auto readNum = [&](double* out) {
        while (q < d.size() && (std::isspace(static_cast<unsigned char>(d[q])) || d[q] == ',')) ++q;
        if (q >= d.size()) return false;
        if (!(std::isdigit(static_cast<unsigned char>(d[q])) || d[q] == '-' || d[q] == '.' || d[q] == '+'))
          return false;
        char* end = nullptr;
        *out = std::strtod(d.c_str() + q, &end);
        q = static_cast<size_t>(end - d.c_str());
        return true;
      };
      auto flush = [&](bool closed) {
        if (cur.size() >= 2) {
          pr.subpaths.push_back(cur);
          pr.closed.push_back(closed);
        }
        cur.clear();
      };
      while (q < d.size()) {
        while (q < d.size() && (std::isspace(static_cast<unsigned char>(d[q])) || d[q] == ',')) ++q;
        if (q >= d.size()) break;
        if (std::isalpha(static_cast<unsigned char>(d[q]))) {
          cmd = d[q++];
          if (cmd == 'Z' || cmd == 'z') {
            flush(true);
            continue;
          }
        }
        double x = 0, y = 0;
        switch (cmd) {
          case 'M':
          case 'm':
            if (!readNum(&x) || !readNum(&y)) {
              q = d.size();
              break;
            }
            flush(false);
            if (cmd == 'm') {
              x += px;
              y += py;
            }
            px = x;
            py = y;
            cur.push_back(pt(px, py));
            cmd = cmd == 'm' ? 'l' : 'L';
            break;
          case 'L':
          case 'l':
            if (!readNum(&x) || !readNum(&y)) {
              q = d.size();
              break;
            }
            if (cmd == 'l') {
              x += px;
              y += py;
            }
            px = x;
            py = y;
            cur.push_back(pt(px, py));
            break;
          case 'H':
          case 'h':
            if (!readNum(&x)) {
              q = d.size();
              break;
            }
            px = cmd == 'h' ? px + x : x;
            cur.push_back(pt(px, py));
            break;
          case 'V':
          case 'v':
            if (!readNum(&y)) {
              q = d.size();
              break;
            }
            py = cmd == 'v' ? py + y : y;
            cur.push_back(pt(px, py));
            break;
          default:
            // curves and arcs: their end point only
            {
              std::vector<double> nums;
              double v;
              while (readNum(&v)) nums.push_back(v);
              if (nums.size() >= 2) {
                x = nums[nums.size() - 2];
                y = nums.back();
                if (std::islower(static_cast<unsigned char>(cmd))) {
                  x += px;
                  y += py;
                }
                px = x;
                py = y;
                cur.push_back(pt(px, py));
              } else {
                q = d.size();
              }
            }
        }
      }
      flush(false);
      pr.kind = Prim::kPath;
      if (!f.hasFill) pr.fill.none = true;
      if (!pr.subpaths.empty()) sc.prims.push_back(pr);
    } else if (name == "text") {
      const size_t close = svg.find("</text>", i);
      std::string content = svg.substr(i, close == std::string::npos ? std::string::npos : close - i);
      // drop nested tags (tspan)
      std::string plain;
      bool inTag = false;
      for (char ch : content) {
        if (ch == '<') inTag = true;
        else if (ch == '>') inTag = false;
        else if (!inTag) plain += ch;
      }
      i = close == std::string::npos ? svg.size() : close + 7;
      pr.kind = Prim::kText;
      auto p0 = pt(num(at, "x"), num(at, "y"));
      pr.cx = p0.first;
      pr.cy = p0.second;
      pr.size = num(at, "font-size", 10) * f.m.scale();
      pr.anchor = str(at, "text-anchor", "start");
      pr.text = xmlUnescape(plain);
      if (!f.hasFill) pr.fill = Rgb{0, 0, 0, false};
      sc.prims.push_back(pr);
    }
  }
  return sc;
}

std::string fnum(double v) {
  char b[48];
  std::snprintf(b, sizeof b, "%.3f", v);
  std::string s = b;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s == "-0") s = "0";
  return s;
}

std::string escapeParen(const std::string& s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch == '(' || ch == ')' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch < 32 || ch > 126) {
      out += '?';
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out;
}

// Helvetica's average advance (0.5 em) for the anchor offset.
double approxWidth(const Prim& p) { return 0.5 * p.size * static_cast<double>(p.text.size()); }

// The shared vector program: PDF and PostScript differ only in operator
// spellings, given here per format. The y axis is flipped once (SVG is y-down).
std::string vectorProgram(const Scene& sc, bool pdf) {
  std::ostringstream o;
  auto color = [&](const Rgb& c, bool fill) {
    o << fnum(c.r) << ' ' << fnum(c.g) << ' ' << fnum(c.b) << ' ' << (pdf ? (fill ? "rg" : "RG") : "setrgbcolor")
      << '\n';
  };
  auto Y = [&](double y) { return sc.height - y; };
  for (const Prim& p : sc.prims) {
    if (p.kind == Prim::kText) {
      if (p.text.empty()) continue;
      double x = p.cx;
      if (p.anchor == "middle") x -= approxWidth(p) / 2;
      else if (p.anchor == "end") x -= approxWidth(p);
      const Rgb c = p.fill.none ? Rgb{} : p.fill;
      color(c, true);
      if (pdf) {
        o << "BT /F1 " << fnum(p.size) << " Tf " << fnum(x) << ' ' << fnum(Y(p.cy)) << " Td (" << escapeParen(p.text)
          << ") Tj ET\n";
      } else {
        o << "/Helvetica findfont " << fnum(p.size) << " scalefont setfont " << fnum(x) << ' ' << fnum(Y(p.cy))
          << " moveto (" << escapeParen(p.text) << ") show\n";
      }
      continue;
    }
    auto pathOut = [&]() {
      if (p.kind == Prim::kCircle) {
        if (pdf) {
          const double k = 0.5522847498 * p.r;
          const double cx = p.cx, cy = Y(p.cy), r = p.r;
          o << fnum(cx + r) << ' ' << fnum(cy) << " m\n";
          o << fnum(cx + r) << ' ' << fnum(cy + k) << ' ' << fnum(cx + k) << ' ' << fnum(cy + r) << ' ' << fnum(cx)
            << ' ' << fnum(cy + r) << " c\n";
          o << fnum(cx - k) << ' ' << fnum(cy + r) << ' ' << fnum(cx - r) << ' ' << fnum(cy + k) << ' '
            << fnum(cx - r) << ' ' << fnum(cy) << " c\n";
          o << fnum(cx - r) << ' ' << fnum(cy - k) << ' ' << fnum(cx - k) << ' ' << fnum(cy - r) << ' ' << fnum(cx)
            << ' ' << fnum(cy - r) << " c\n";
          o << fnum(cx + k) << ' ' << fnum(cy - r) << ' ' << fnum(cx + r) << ' ' << fnum(cy - k) << ' '
            << fnum(cx + r) << ' ' << fnum(cy) << " c h\n";
        } else {
          o << "newpath " << fnum(p.cx) << ' ' << fnum(Y(p.cy)) << ' ' << fnum(p.r) << " 0 360 arc closepath\n";
        }
        return;
      }
      if (!pdf) o << "newpath\n";
      for (size_t s = 0; s < p.subpaths.size(); ++s) {
        const auto& sp = p.subpaths[s];
        for (size_t k = 0; k < sp.size(); ++k) {
          o << fnum(sp[k].first) << ' ' << fnum(Y(sp[k].second)) << ' '
            << (k == 0 ? (pdf ? "m" : "moveto") : (pdf ? "l" : "lineto")) << '\n';
        }
        if (p.closed[s]) o << (pdf ? "h\n" : "closepath\n");
      }
    };
    if (!p.fill.none) {
      color(p.fill, true);
      pathOut();
      o << (pdf ? "f\n" : "fill\n");
    }
    if (!p.stroke.none && p.width > 0) {
      color(p.stroke, false);
      o << fnum(p.width) << (pdf ? " w\n" : " setlinewidth\n");
      pathOut();
      o << (pdf ? "S\n" : "stroke\n");
    }
  }
  return o.str();
}

std::string toPdf(const Scene& sc) {
  const std::string content = vectorProgram(sc, true);
  std::vector<std::string> objs;
  objs.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objs.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  objs.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + fnum(sc.width) + " " + fnum(sc.height) +
                 "] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>");
  objs.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
  objs.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offs;
  for (size_t k = 0; k < objs.size(); ++k) {
    offs.push_back(out.size());
    out += std::to_string(k + 1) + " 0 obj\n" + objs[k] + "\nendobj\n";
  }
  const size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
  for (size_t o : offs) {
    char b[32];
    std::snprintf(b, sizeof b, "%010zu 00000 n \n", o);
    out += b;
  }
  out += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return out;
}

std::string toEps(const Scene& sc) {
  std::string out = "%!PS-Adobe-3.0 EPSF-3.0\n%%Creator: PIET\n%%BoundingBox: 0 0 " +
                    std::to_string(static_cast<int>(std::ceil(sc.width))) + " " +
                    std::to_string(static_cast<int>(std::ceil(sc.height))) + "\n%%Pages: 1\n%%EndComments\n";
  out += "gsave\n1 setlinejoin 1 setlinecap\n";
  out += vectorProgram(sc, false);
  out += "grestore\nshowpage\n%%EOF\n";
  return out;
}

// ---- PNG ----

uint32_t crc32Of(const unsigned char* p, size_t n, uint32_t crc = 0xffffffffu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return crc;
}

void put32(std::string& s, uint32_t v) {
  s += static_cast<char>((v >> 24) & 0xff);
  s += static_cast<char>((v >> 16) & 0xff);
  s += static_cast<char>((v >> 8) & 0xff);
  s += static_cast<char>(v & 0xff);
}

void pngChunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td = std::string(type, 4) + data;
  out += td;
  const uint32_t crc = crc32Of(reinterpret_cast<const unsigned char*>(td.data()), td.size()) ^ 0xffffffffu;
  put32(out, crc);
}

struct Canvas {
  int w, h;
  std::vector<unsigned char> px;  // RGB
  Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * static_cast<size_t>(h_) * 3, 255) {}
  void set(int x, int y, const Rgb& c) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 3;
    px[i] = static_cast<unsigned char>(std::lround(c.r * 255));
    px[i + 1] = static_cast<unsigned char>(std::lround(c.g * 255));
    px[i + 2] = static_cast<unsigned char>(std::lround(c.b * 255));
  }
  void dot(double x, double y, double rad, const Rgb& c) {
    const int r = std::max(0, static_cast<int>(std::ceil(rad)));
    const int cx = static_cast<int>(std::lround(x)), cy = static_cast<int>(std::lround(y));
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx)
        if (r == 0 || dx * dx + dy * dy <= r * r) set(cx + dx, cy + dy, c);
  }
  void line(double x0, double y0, double x1, double y1, double width, const Rgb& c) {
    const double len = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::max(1, static_cast<int>(std::ceil(len * 2)));
    const double rad = std::max(0.0, width / 2 - 0.5);
    for (int s = 0; s <= steps; ++s) {
      const double t = static_cast<double>(s) / steps;
      dot(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, rad, c);
    }
  }
  // even-odd scanline fill of closed polygons
  void fillPolys(const std::vector<std::vector<std::pair<double, double>>>& polys, const Rgb& c) {
    double ymin = 1e300, ymax = -1e300;
    for (const auto& p : polys)
      for (const auto& q : p) {
        ymin = std::min(ymin, q.second);
        ymax = std::max(ymax, q.second);
      }
    if (ymin > ymax) return;
    const int y0 = std::max(0, static_cast<int>(std::floor(ymin))), y1 = std::min(h - 1, static_cast<int>(std::ceil(ymax)));
    for (int y = y0; y <= y1; ++y) {
      const double sy = y + 0.5;
      std::vector<double> xs;
      for (const auto& p : polys) {
        for (size_t k = 0; k < p.size(); ++k) {
          const auto& a = p[k];
          const auto& b = p[(k + 1) % p.size()];
          if ((a.second <= sy && b.second > sy) || (b.second <= sy && a.second > sy)) {
            xs.push_back(a.first + (sy - a.second) * (b.first - a.first) / (b.second - a.second));
          }
        }
      }
      std::sort(xs.begin(), xs.end());
      for (size_t k = 0; k + 1 < xs.size(); k += 2) {
        for (int x = static_cast<int>(std::ceil(xs[k] - 0.5)); x <= static_cast<int>(std::floor(xs[k + 1] - 0.5)); ++x)
          set(x, y, c);
      }
    }
  }
};

Canvas rasterize(const Scene& sc, int width, int height) {
  if (width <= 0 && height <= 0) width = static_cast<int>(std::lround(sc.width * 2));
  if (width <= 0) width = static_cast<int>(std::lround(sc.width * height / sc.height));
  if (height <= 0) height = static_cast<int>(std::lround(sc.height * width / sc.width));
  width = std::clamp(width, 1, 16000);
  height = std::clamp(height, 1, 16000);
  Canvas cv(width, height);
  const double sx = width / sc.width, sy = height / sc.height;
  const double s = std::min(sx, sy);
  for (const Prim& p : sc.prims) {
    if (p.kind == Prim::kText) continue;
    if (p.kind == Prim::kCircle) {
      if (!p.fill.none) cv.dot(p.cx * sx, p.cy * sy, p.r * s, p.fill);
      if (!p.stroke.none) {
        const int n = std::max(16, static_cast<int>(p.r * s * 6));
        for (int k = 0; k < n; ++k) {
          const double a0 = 2 * M_PI * k / n, a1 = 2 * M_PI * (k + 1) / n;
          cv.line((p.cx + p.r * std::cos(a0)) * sx, (p.cy + p.r * std::sin(a0)) * sy,
                  (p.cx + p.r * std::cos(a1)) * sx, (p.cy + p.r * std::sin(a1)) * sy, p.width * s, p.stroke);
        }
      }
      continue;
    }
    std::vector<std::vector<std::pair<double, double>>> scaled;
    for (const auto& sp : p.subpaths) {
      std::vector<std::pair<double, double>> v;
      for (const auto& q : sp) v.emplace_back(q.first * sx, q.second * sy);
      scaled.push_back(v);
    }
    if (!p.fill.none) cv.fillPolys(scaled, p.fill);
    if (!p.stroke.none && p.width > 0) {
      for (size_t k = 0; k < scaled.size(); ++k) {
        const auto& v = scaled[k];
        for (size_t q = 0; q + 1 < v.size(); ++q)
          cv.line(v[q].first, v[q].second, v[q + 1].first, v[q + 1].second, p.width * s, p.stroke);
        if (p.closed[k] && v.size() > 2)
          cv.line(v.back().first, v.back().second, v.front().first, v.front().second, p.width * s, p.stroke);
      }
    }
  }
  return cv;
}

std::string toPng(const Scene& sc, int widthIn, int heightIn) {
  const Canvas cv = rasterize(sc, widthIn, heightIn);
  const int width = cv.w, height = cv.h;
  // raw scanlines (filter 0) in stored deflate blocks
  std::string raw;
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
  for (int y = 0; y < height; ++y) {
    raw += '\0';
    raw.append(reinterpret_cast<const char*>(&cv.px[static_cast<size_t>(y) * static_cast<size_t>(width) * 3]),
               static_cast<size_t>(width) * 3);
  }
  std::string z;
  z += static_cast<char>(0x78);
  z += static_cast<char>(0x01);
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z += static_cast<char>(last ? 1 : 0);
    z += static_cast<char>(n & 0xff);
    z += static_cast<char>((n >> 8) & 0xff);
    z += static_cast<char>(~n & 0xff);
    z += static_cast<char>((~n >> 8) & 0xff);
    z.append(raw, pos, n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) {
    a = (a + ch) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  put32(ihdr, static_cast<uint32_t>(width));
  put32(ihdr, static_cast<uint32_t>(height));
  ihdr += static_cast<char>(8);  // bit depth
  ihdr += static_cast<char>(2);  // RGB
  ihdr += '\0';
  ihdr += '\0';
  ihdr += '\0';
  pngChunk(out, "IHDR", ihdr);
  pngChunk(out, "IDAT", z);
  pngChunk(out, "IEND", std::string());
  return out;
}


// ---------------------------------------------------------------------------
// JPEG (baseline, 4:4:4, quality-scaled standard tables). `graph export x.jpg`
// is a raster format Stata writes (007's figures export .jpg). This encoder
// follows the baseline process of ITU-T T.81 with the example quantization and
// Huffman tables of its Annex K, scaled by quality the way the IJG library
// defines it; no external library.

const int kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                         12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                         58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const int kLumQ[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                       14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                       18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                       49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const int kChrQ[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                       24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                       99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
// Annex K.3 tables: code counts per length 1..16, then the symbols.
const unsigned char kDcLumBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const unsigned char kDcLumVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char kDcChrBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const unsigned char kDcChrVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const unsigned char kAcLumBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const unsigned char kAcLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
const unsigned char kAcChrBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const unsigned char kAcChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct HuffTable {
  uint16_t code[256] = {};
  uint8_t len[256] = {};
};

HuffTable buildHuff(const unsigned char* bits, const unsigned char* vals) {
  HuffTable t;
  uint16_t code = 0;
  int k = 0;
  for (int l = 1; l <= 16; ++l) {
    for (int i = 0; i < bits[l - 1]; ++i, ++k) {
      t.code[vals[k]] = code++;
      t.len[vals[k]] = static_cast<uint8_t>(l);
    }
    code <<= 1;
  }
  return t;
}

struct BitWriter {
  std::string* out;
  uint32_t acc = 0;
  int n = 0;
  void put(uint32_t bits, int len) {
    for (int i = len - 1; i >= 0; --i) {
      acc = (acc << 1) | ((bits >> i) & 1u);
      if (++n == 8) {
        out->push_back(static_cast<char>(acc & 0xff));
        if ((acc & 0xff) == 0xff) out->push_back('\0');  // byte stuffing
        acc = 0;
        n = 0;
      }
    }
  }
  void flush() {
    while (n != 0) put(1, 1);  // pad with 1-bits
  }
};

void putMarker16(std::string& s, int v) {
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

std::string toJpeg(const Scene& sc, int widthIn, int heightIn, int quality) {
  const Canvas cv = rasterize(sc, widthIn, heightIn);
  const int W = cv.w, H = cv.h;
  quality = std::clamp(quality, 1, 100);
  const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
  int qY[64], qC[64];
  for (int i = 0; i < 64; ++i) {
    qY[i] = std::clamp((kLumQ[i] * scale + 50) / 100, 1, 255);
    qC[i] = std::clamp((kChrQ[i] * scale + 50) / 100, 1, 255);
  }
  std::string out;
  out += "\xFF\xD8";                                              // SOI
  out += "\xFF\xE0";                                              // APP0 JFIF
  putMarker16(out, 16);
  out += std::string("JFIF\0", 5);
  out += "\x01\x01";
  out += '\0';                                                    // no units
  putMarker16(out, 1);
  putMarker16(out, 1);
  out += '\0';
  out += '\0';
  for (int t = 0; t < 2; ++t) {                                   // DQT
    out += "\xFF\xDB";
    putMarker16(out, 67);
    out += static_cast<char>(t);
    for (int i = 0; i < 64; ++i) out += static_cast<char>((t == 0 ? qY : qC)[kZigzag[i]]);
  }
  out += "\xFF\xC0";                                              // SOF0
  putMarker16(out, 17);
  out += static_cast<char>(8);
  putMarker16(out, H);
  putMarker16(out, W);
  out += static_cast<char>(3);
  for (int c = 0; c < 3; ++c) {
    out += static_cast<char>(c + 1);
    out += static_cast<char>(0x11);                               // 4:4:4
    out += static_cast<char>(c == 0 ? 0 : 1);
  }
  auto dht = [&](int cls, int id, const unsigned char* bits, const unsigned char* vals, int nvals) {
    out += "\xFF\xC4";
    putMarker16(out, 19 + nvals);
    out += static_cast<char>((cls << 4) | id);
    for (int i = 0; i < 16; ++i) out += static_cast<char>(bits[i]);
    for (int i = 0; i < nvals; ++i) out += static_cast<char>(vals[i]);
  };
  dht(0, 0, kDcLumBits, kDcLumVal, 12);
  dht(1, 0, kAcLumBits, kAcLumVal, 162);
  dht(0, 1, kDcChrBits, kDcChrVal, 12);
  dht(1, 1, kAcChrBits, kAcChrVal, 162);
  out += "\xFF\xDA";                                              // SOS
  putMarker16(out, 12);
  out += static_cast<char>(3);
  for (int c = 0; c < 3; ++c) {
    out += static_cast<char>(c + 1);
    out += static_cast<char>(c == 0 ? 0x00 : 0x11);
  }
  out += '\0';
  out += static_cast<char>(63);
  out += '\0';

  const HuffTable dcY = buildHuff(kDcLumBits, kDcLumVal), acY = buildHuff(kAcLumBits, kAcLumVal);
  const HuffTable dcC = buildHuff(kDcChrBits, kDcChrVal), acC = buildHuff(kAcChrBits, kAcChrVal);
  double cosT[8][8];
  for (int x = 0; x < 8; ++x)
    for (int u = 0; u < 8; ++u) cosT[x][u] = std::cos((2 * x + 1) * u * M_PI / 16.0);
  BitWriter bw{&out};
  int prevDc[3] = {0, 0, 0};
  auto magnitude = [](int v, int* nbits) -> uint32_t {
    int a = v < 0 ? -v : v;
    int n = 0;
    while (a) {
      ++n;
      a >>= 1;
    }
    *nbits = n;
    return static_cast<uint32_t>(v < 0 ? v + (1 << n) - 1 : v);
  };
  for (int by = 0; by < H; by += 8) {
    for (int bx = 0; bx < W; bx += 8) {
      double blk[3][64];
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          const int px = std::min(bx + x, W - 1), py = std::min(by + y, H - 1);
          const size_t i = (static_cast<size_t>(py) * static_cast<size_t>(W) + static_cast<size_t>(px)) * 3;
          const double r = cv.px[i], g = cv.px[i + 1], b = cv.px[i + 2];
          blk[0][y * 8 + x] = 0.299 * r + 0.587 * g + 0.114 * b - 128;
          blk[1][y * 8 + x] = -0.168736 * r - 0.331264 * g + 0.5 * b;
          blk[2][y * 8 + x] = 0.5 * r - 0.418688 * g - 0.081312 * b;
        }
      }
      for (int c = 0; c < 3; ++c) {
        const int* q = c == 0 ? qY : qC;
        int coef[64];
        for (int v = 0; v < 8; ++v) {
          for (int u = 0; u < 8; ++u) {
            double sum = 0;
            for (int y = 0; y < 8; ++y)
              for (int x = 0; x < 8; ++x) sum += blk[c][y * 8 + x] * cosT[x][u] * cosT[y][v];
            const double cu = u == 0 ? M_SQRT1_2 : 1.0, cv2 = v == 0 ? M_SQRT1_2 : 1.0;
            const double f = 0.25 * cu * cv2 * sum;
            coef[v * 8 + u] = static_cast<int>(std::lround(f / q[v * 8 + u]));
          }
        }
        const HuffTable& dc = c == 0 ? dcY : dcC;
        const HuffTable& ac = c == 0 ? acY : acC;
        int nb = 0;
        const int diff = coef[0] - prevDc[c];
        prevDc[c] = coef[0];
        const uint32_t dbits = magnitude(diff, &nb);
        bw.put(dc.code[nb], dc.len[nb]);
        if (nb) bw.put(dbits, nb);
        int run = 0;
        for (int k = 1; k < 64; ++k) {
          const int v = coef[kZigzag[k]];
          if (v == 0) {
            ++run;
            continue;
          }
          while (run > 15) {
            bw.put(ac.code[0xF0], ac.len[0xF0]);  // ZRL
            run -= 16;
          }
          const uint32_t abits = magnitude(v, &nb);
          const int sym = (run << 4) | nb;
          bw.put(ac.code[sym], ac.len[sym]);
          bw.put(abits, nb);
          run = 0;
        }
        if (run > 0) bw.put(ac.code[0x00], ac.len[0x00]);  // EOB
      }
    }
  }
  bw.flush();
  out += "\xFF\xD9";                                              // EOI
  return out;
}

}  // namespace

bool graphSvgToFormat(const std::string& svg, const std::string& fmt, int widthPx, int heightPx,
                      std::string* out) {
  const Scene sc = parseSvg(svg);
  if (fmt == "pdf") {
    *out = toPdf(sc);
    return true;
  }
  if (fmt == "eps" || fmt == "ps") {
    *out = toEps(sc);
    return true;
  }
  if (fmt == "png") {
    *out = toPng(sc, widthPx, heightPx);
    return true;
  }
  if (fmt == "jpg" || fmt == "jpeg") {
    *out = toJpeg(sc, widthPx, heightPx, 90);
    return true;
  }
  return false;
}

}  // namespace piet::handlers
