#ifndef GNURADIO_TOOLS_DOCWRITER_HPP
#define GNURADIO_TOOLS_DOCWRITER_HPP

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gr::tools {

enum class Format { Markdown, Html };

[[nodiscard]] inline std::optional<Format> parseFormat(std::string_view text) noexcept {
    if (text == "md" || text == "markdown") {
        return Format::Markdown;
    }
    if (text == "html") {
        return Format::Html;
    }
    return std::nullopt;
}

/**
 * @brief Emits one document as either CommonMark or a single self-contained HTML page.
 *
 * The caller describes the document structurally -- headings, lists, tables, diagrams -- and
 * the writer spells it for the chosen format. The HTML page carries its style sheet inline and
 * references nothing outside itself, so it renders with the network unavailable; a diagram
 * reaches it as an inline `svg` the caller drew, which a browser draws with no script and no
 * library. Markdown carries a diagram as mermaid source in a fenced block, which a
 * mermaid-aware reader draws and any other reader shows as legible text.
 *
 * Anchors are minted here rather than left to the renderer, because CommonMark has no anchor
 * syntax and a heading-derived anchor differs between renderers. Markdown headings therefore
 * carry an explicit HTML anchor element, which every CommonMark renderer passes through.
 */
class DocWriter {
    Format                     _format;
    std::string                _title;
    std::string                _body;
    std::map<std::string, int> _anchorUse;

public:
    DocWriter(Format format, std::string title) : _format(format), _title(std::move(title)) {}

    /// the format the document is being spelled in; a caller that draws a diagram picks its form from it
    [[nodiscard]] Format format() const noexcept { return _format; }

    /// escapes text for a table cell: a newline becomes a line break, and the cell separator and
    /// the markup characters of the target format lose their meaning
    [[nodiscard]] std::string cell(std::string_view text) const {
        std::string out;
        out.reserve(text.size());
        for (const char c : text) {
            if (c == '\n') {
                out += "<br>";
                continue;
            }
            if (_format == Format::Html) {
                switch (c) {
                case '&': out += "&amp;"; continue;
                case '<': out += "&lt;"; continue;
                case '>': out += "&gt;"; continue;
                case '"': out += "&quot;"; continue;
                default: break;
                }
            } else {
                if (c == '|' || c == '\\') {
                    out += '\\';
                }
            }
            out += c;
        }
        return out;
    }

    /// Escapes text for prose, a heading or a link label; a newline stays a newline.
    ///
    /// The underscore is deliberately left alone: CommonMark does not read one inside a word as
    /// emphasis, and every identifier this project prints is snake_case, so escaping it would
    /// make the source of the document unreadable to no purpose. The angle brackets are not
    /// optional, though -- `Block<float32>` unescaped is raw HTML, and a renderer swallows it.
    [[nodiscard]] std::string text(std::string_view source) const {
        std::string out;
        out.reserve(source.size());
        for (const char c : source) {
            if (_format == Format::Html) {
                switch (c) {
                case '&': out += "&amp;"; continue;
                case '<': out += "&lt;"; continue;
                case '>': out += "&gt;"; continue;
                case '"': out += "&quot;"; continue;
                default: break;
                }
            } else {
                if (c == '\\' || c == '`' || c == '*' || c == '[' || c == ']' || c == '<' || c == '>') {
                    out += '\\';
                }
            }
            out += c;
        }
        return out;
    }

    /// a slug of the given text, made unique within the document by a numeric suffix
    [[nodiscard]] std::string anchor(std::string_view source) {
        std::string slug;
        slug.reserve(source.size());
        for (const char c : source) {
            const auto uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) != 0) {
                slug += static_cast<char>(std::tolower(uc));
            } else if (!slug.empty() && slug.back() != '-') {
                slug += '-';
            }
        }
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
        if (slug.empty()) {
            slug = "section";
        }
        const int seen = _anchorUse[slug]++;
        return seen == 0 ? slug : std::format("{}-{}", slug, seen);
    }

    void heading(std::size_t level, std::string_view headingText, std::string_view anchorName = {}) {
        if (_format == Format::Html) {
            const std::size_t tag = std::min<std::size_t>(level, 6UZ);
            if (anchorName.empty()) {
                _body += std::format("<h{0}>{1}</h{0}>\n", tag, text(headingText));
            } else {
                _body += std::format("<h{0} id=\"{1}\">{2}</h{0}>\n", tag, anchorName, text(headingText));
            }
        } else {
            if (!anchorName.empty()) {
                _body += std::format("<a id=\"{}\"></a>\n\n", anchorName);
            }
            _body += std::format("{} {}\n\n", std::string(std::min<std::size_t>(level, 6UZ), '#'), text(headingText));
        }
    }

    void paragraph(std::string_view prose) {
        if (prose.empty()) {
            return;
        }
        _body += _format == Format::Html ? std::format("<p>{}</p>\n", text(prose)) : std::format("{}\n\n", text(prose));
    }

    /// a bullet list whose items are already escaped, so a caller may put emphasis in them
    void rawBullets(std::span<const std::string> items) {
        if (items.empty()) {
            return;
        }
        if (_format == Format::Html) {
            _body += "<ul>\n";
            for (const std::string& item : items) {
                _body += std::format("<li>{}</li>\n", item);
            }
            _body += "</ul>\n";
        } else {
            for (const std::string& item : items) {
                _body += std::format("- {}\n", item);
            }
            _body += "\n";
        }
    }

    /// a name and its value on one line, the name emphasized
    [[nodiscard]] std::string labeled(std::string_view name, std::string_view value) const { return _format == Format::Html ? std::format("<strong>{}</strong>: {}", text(name), text(value)) : std::format("**{}**: {}", text(name), text(value)); }

    [[nodiscard]] std::string link(std::string_view label, std::string_view anchorName) const { //
        return _format == Format::Html ? std::format("<a href=\"#{}\">{}</a>", anchorName, text(label)) : std::format("[{}](#{})", text(label), anchorName);
    }

    /// Rows carry raw cell text; the writer escapes each one. A `bounded` table is given a box of
    /// its own that scrolls in both directions past a fixed height, its header row staying in view,
    /// so that a long table does not push what follows it off the page. Markdown has one table
    /// shape and ignores the request.
    void table(std::span<const std::string_view> headers, std::span<const std::vector<std::string>> rows, bool bounded = false) {
        if (headers.empty()) {
            return;
        }
        if (_format == Format::Html) {
            _body += bounded ? "<div class=\"table bounded\">\n<table>\n<thead>\n<tr>" : "<div class=\"table\">\n<table>\n<thead>\n<tr>";
            for (const std::string_view header : headers) {
                _body += std::format("<th>{}</th>", cell(header));
            }
            _body += "</tr>\n</thead>\n<tbody>\n";
            for (const std::vector<std::string>& row : rows) {
                _body += "<tr>";
                for (std::size_t column = 0UZ; column < headers.size(); ++column) {
                    _body += std::format("<td>{}</td>", column < row.size() ? cell(row[column]) : std::string{});
                }
                _body += "</tr>\n";
            }
            _body += "</tbody>\n</table>\n</div>\n";
        } else {
            _body += "|";
            for (const std::string_view header : headers) {
                _body += std::format(" {} |", cell(header));
            }
            _body += "\n|";
            for (std::size_t column = 0UZ; column < headers.size(); ++column) {
                _body += " --- |";
            }
            _body += "\n";
            for (const std::vector<std::string>& row : rows) {
                _body += "|";
                for (std::size_t column = 0UZ; column < headers.size(); ++column) {
                    _body += std::format(" {} |", column < row.size() ? cell(row[column]) : std::string{});
                }
                _body += "\n";
            }
            _body += "\n";
        }
    }

    /// a mermaid diagram, which a Markdown reader draws from its fenced source
    void mermaid(std::string_view source) { _body += std::format("```mermaid\n{}```\n\n", source); }

    /// A drawn diagram, placed in the page as it stands. The caller owns the markup, including the
    /// escaping of its labels; the page's style sheet declares the classes it draws with. The box
    /// around it scrolls sideways, as the one around a table does, so a drawing too wide to be
    /// legible at the page's width can keep its own size instead.
    void svg(std::string_view markup) { _body += std::format("<div class=\"diagram\">\n{}</div>\n", markup); }

    [[nodiscard]] std::string finish() const {
        if (_format == Format::Markdown) {
            // the blank line that separates blocks is a separator, not part of the last one
            std::string_view body(_body);
            body.remove_suffix(body.size() - std::min(body.size(), body.find_last_not_of("\n") + 1UZ));
            return std::format("# {}\n\n{}\n", _title, body);
        }
        return std::format(R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{0}</title>
<style>
:root {{ color-scheme: light dark; --fg: #1a1a1a; --bg: #ffffff; --muted: #5a6270; --rule: #d5d9e0; --panel: #f5f6f8; --accent: #1f5fa9; }}
@media (prefers-color-scheme: dark) {{
  :root {{ --fg: #e6e6e6; --bg: #14171c; --muted: #9aa4b2; --rule: #2c313a; --panel: #1c2027; --accent: #7fb2f0; }}
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0 auto; padding: 2rem 1.25rem 4rem; max-width: 60rem; background: var(--bg); color: var(--fg);
       font: 15px/1.6 -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; }}
h1, h2, h3, h4, h5, h6 {{ line-height: 1.25; margin: 2rem 0 0.75rem; }}
h1 {{ font-size: 1.9rem; margin-top: 0; padding-bottom: 0.4rem; border-bottom: 2px solid var(--rule); }}
h2 {{ font-size: 1.4rem; padding-bottom: 0.3rem; border-bottom: 1px solid var(--rule); }}
h3 {{ font-size: 1.15rem; }}
h4, h5, h6 {{ font-size: 1rem; color: var(--muted); }}
p, ul {{ margin: 0.6rem 0; }}
ul {{ padding-left: 1.4rem; }}
a {{ color: var(--accent); }}
code, pre {{ font-family: ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas, monospace; font-size: 0.87em; }}
code {{ background: var(--panel); padding: 0.1em 0.35em; border-radius: 3px; }}
pre {{ background: var(--panel); border: 1px solid var(--rule); border-radius: 5px; padding: 0.9rem 1rem; overflow-x: auto; }}
pre code {{ background: none; padding: 0; }}
div.table {{ overflow-x: auto; }}
div.table.bounded {{ max-height: 60vh; overflow: auto; margin: 0.8rem 0; }}
div.table.bounded table {{ margin: 0; }}
/* a collapsed border does not travel with a sticky cell, so the header keeps its rule as a shadow */
div.table.bounded thead th {{ position: sticky; top: 0; z-index: 1; box-shadow: inset 0 -1px var(--rule); }}
table {{ border-collapse: collapse; width: 100%; margin: 0.8rem 0; }}
th, td {{ border: 1px solid var(--rule); padding: 0.35rem 0.6rem; text-align: left; vertical-align: top; }}
th {{ background: var(--panel); font-weight: 600; }}
tbody tr:nth-child(even) {{ background: color-mix(in srgb, var(--panel) 45%, transparent); }}
div.diagram {{ overflow-x: auto; }}
svg.flowgraph {{ display: block; margin: 0.9rem 0; }}
svg.flowgraph .node {{ fill: var(--panel); stroke: var(--muted); stroke-width: 1.2; }}
svg.flowgraph .node.unresolved {{ stroke-dasharray: 5 4; }}
svg.flowgraph .inner {{ fill: none; stroke: var(--muted); stroke-width: 1; }}
svg.flowgraph .name {{ fill: var(--fg); font: 600 13px -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; }}
svg.flowgraph .type {{ fill: var(--muted); font: 11px ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas, monospace; }}
svg.flowgraph .port {{ fill: var(--muted); font: 10px ui-monospace, "SF Mono", "Cascadia Mono", Menlo, Consolas, monospace; }}
svg.flowgraph .edge {{ fill: none; stroke: var(--accent); stroke-width: 1.3; }}
svg.flowgraph .arrow {{ fill: var(--accent); }}
</style>
</head>
<body>
<h1>{0}</h1>
{1}</body>
</html>
)",
            text(_title), _body);
    }
};

} // namespace gr::tools

#endif // GNURADIO_TOOLS_DOCWRITER_HPP
