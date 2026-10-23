#include "Sidecar.h"

#include "LocalAdjustmentCodec.h"
#include "RawImport.h"
#include "SettingCodec.h"
#include "SidecarLimits.h"

#include <BrushStrokes.h>
#include <NegativeSettings.h>
#include <SettingsJson.h>
#include <ShortestDecimal.h>
#include <Snapshots.h>

#include <QByteArray>
#include <QDateTime>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNamedNodeMap>
#include <QDomNodeList>
#include <QDomProcessingInstruction>
#include <QFile>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>
#include <QTimeZone>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace arraw;
using namespace Qt::StringLiterals;

namespace {

constexpr auto rdfNamespace = QLatin1StringView("http://www.w3.org/1999/02/22-rdf-syntax-ns#");
constexpr auto xmpNamespace = QLatin1StringView("http://ns.adobe.com/xap/1.0/");
constexpr auto xmpPrefix = QLatin1StringView("xmp");

/// @brief Wraps a new sidecar, as XMP packets are wrapped; the BOM is UTF-8's.
constexpr auto newSidecar = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Arraw\">\n"
                            " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                            " </rdf:RDF>\n"
                            "</x:xmpmeta>\n"
                            "<?xpacket end=\"w\"?>\n";

/// @brief Names a property by its namespace, the prefix a new one gets, and its local name.
struct Property {
    QString ns;
    QString prefix;
    QString local;
};

/// @brief Names an arraw property.
Property arrawProperty(std::string_view key) {
    return {QString::fromUtf8(sidecarNamespace.data(), sidecarNamespace.size()),
            QString::fromUtf8(sidecarPrefix.data(), sidecarPrefix.size()),
            QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()))};
}

/// @brief Names a property of the standard XMP namespace.
Property xmpProperty(QLatin1StringView local) {
    return {xmpNamespace, xmpPrefix, QString(local)};
}

/// @brief Spells a path the way Qt takes it.
QString qtPath(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

/// @brief Drops the one leading `+` XMP writes before a positive number, which `from_chars`
/// refuses.
std::string_view withoutPlus(std::string_view text) {
    return text.starts_with('+') ? text.substr(1) : text;
}

/// @brief Extensions of the other image formats that can share a stem with a photograph.
constexpr std::array<std::string_view, 12> otherExtensions = {".jpg",  ".jpeg", ".png",  ".tif",
                                                              ".tiff", ".heic", ".heif", ".webp",
                                                              ".avif", ".jxl",  ".bmp",  ".gif"};

/// @brief One place a property appears in a document, as an attribute or as a child element.
struct Occurrence {
    QDomElement holder;  ///< The description it is on.
    QDomElement element; ///< The child element, null for an attribute.
    QString name;        ///< The attribute's qualified name, empty for an element.
    QString text;        ///< Its value, trimmed.
    bool simple = true;  ///< Whether it holds text alone.
};

/// @brief A property by the namespace its prefix resolved to, and its local name.
struct Resolved {
    QString ns;
    QString local;
};

// The DOM is parsed without namespace processing. Qt's namespace-aware mode does not write a
// document back the way it read it: it moves and duplicates declarations, and can drop a prefix.
// Prefixes are resolved here, through the `xmlns` attributes in scope.

/// @brief Finds the namespace a prefix means where an element stands; empty when it is unbound.
///
/// An empty prefix asks for the default namespace.
QString namespaceOf(const QDomElement& scope, const QString& prefix) {
    const QString declaration = prefix.isEmpty() ? QString("xmlns") : "xmlns:" + prefix;
    for (QDomNode node = scope; node.isElement(); node = node.parentNode()) {
        if (node.toElement().hasAttribute(declaration)) {
            return node.toElement().attribute(declaration);
        }
    }
    return {};
}

/// @brief Splits a qualified name at its colon.
std::pair<QString, QString> splitName(const QString& name) {
    const qsizetype colon = name.indexOf(':');
    return colon < 0 ? std::pair{QString(), name}
                     : std::pair{name.left(colon), name.mid(colon + 1)};
}

/// @brief Resolves an element's name.
Resolved resolved(const QDomElement& element) {
    const auto [prefix, local] = splitName(element.tagName());
    return {namespaceOf(element, prefix), local};
}

/// @brief Lists the properties a description holds, attributes before child elements.
///
/// An attribute without a prefix is in no namespace, and a declaration is not a property.
std::vector<Resolved> propertiesOf(const QDomElement& description) {
    std::vector<Resolved> properties;
    const QDomNamedNodeMap attributes = description.attributes();
    for (int i = 0; i < attributes.size(); ++i) {
        const auto [prefix, local] = splitName(attributes.item(i).nodeName());
        if (!prefix.isEmpty() && prefix != "xmlns"_L1) {
            properties.push_back({namespaceOf(description, prefix), local});
        }
    }
    for (QDomElement child = description.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        properties.push_back(resolved(child));
    }
    return properties;
}

/// @brief Namespaces that are the packet's own or arraw's, not another tool's.
///
/// An empty namespace, which is what an unbound prefix resolves to, counts as
/// own and is dropped silently. The XML namespace entry is a safeguard only:
/// `xml:` is implicitly bound and never reaches here as a declared prefix.
bool isOwnNamespace(const QString& ns) {
    static const std::array<QLatin1StringView, 6> own = {
        rdfNamespace,
        xmpNamespace,
        QLatin1StringView("adobe:ns:meta/"),
        QLatin1StringView("http://www.w3.org/2000/xmlns/"),
        QLatin1StringView("http://www.w3.org/XML/1998/namespace"),
        QLatin1StringView(sidecarNamespace.data(),
                          static_cast<qsizetype>(sidecarNamespace.size()))};
    return ns.isEmpty() || std::ranges::find(own, ns) != own.end();
}

/// @brief Lists the other tools' namespaces and how many properties each holds.
///
/// Attributes are taken by name and then the child elements in document order,
/// for Qt keeps attributes unordered.
std::vector<ForeignNamespace> foreignNamespacesIn(const std::vector<QDomElement>& descriptions) {
    std::vector<ForeignNamespace> others;
    const auto count = [&](const QString& ns, const QString& prefix) {
        if (isOwnNamespace(ns)) {
            return;
        }
        const std::string uri = ns.toStdString();
        auto found = std::ranges::find(others, uri, &ForeignNamespace::uri);
        if (found == others.end()) {
            others.push_back({uri, prefix.toStdString(), 0});
            found = others.end() - 1;
        }
        ++found->properties;
    };
    for (const QDomElement& description : descriptions) {
        const QDomNamedNodeMap attributes = description.attributes();
        QStringList names;
        for (int i = 0; i < attributes.size(); ++i) {
            names.push_back(attributes.item(i).nodeName());
        }
        names.sort();
        for (const QString& name : names) {
            const auto [prefix, local] = splitName(name);
            if (!prefix.isEmpty() && prefix != "xmlns"_L1) {
                count(namespaceOf(description, prefix), prefix);
            }
        }
        for (QDomElement child = description.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            count(resolved(child).ns, splitName(child.tagName()).first);
        }
    }
    return others;
}

/// @brief Whether an element sits in a namespace under a local name.
bool isNamed(const QDomElement& element, QLatin1StringView ns, QLatin1StringView local) {
    const Resolved name = resolved(element);
    return name.ns == ns && name.local == local;
}

/// @brief Collects the elements below a node that are named so, in document order.
void collectNamed(const QDomElement& node, QLatin1StringView ns, QLatin1StringView local,
                  std::vector<QDomElement>& into) {
    if (isNamed(node, ns, local)) {
        into.push_back(node);
    }
    for (QDomElement child = node.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        collectNamed(child, ns, local, into);
    }
}

/// @brief Finds every `rdf:RDF` in a document.
std::vector<QDomElement> packetsOf(const QDomDocument& document) {
    std::vector<QDomElement> packets;
    collectNamed(document.documentElement(), rdfNamespace, "RDF"_L1, packets);
    return packets;
}

/// @brief Finds every `rdf:Description` directly inside an `rdf:RDF`, in document order.
std::vector<QDomElement> descriptionsOf(const QDomDocument& document) {
    std::vector<QDomElement> descriptions;
    for (const QDomElement& packet : packetsOf(document)) {
        for (QDomElement child = packet.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            if (isNamed(child, rdfNamespace, "Description"_L1)) {
                descriptions.push_back(child);
            }
        }
    }
    return descriptions;
}

/// @brief Finds every place a property appears, attributes before elements within a description.
std::vector<Occurrence> occurrencesOf(const std::vector<QDomElement>& descriptions,
                                      const Property& property) {
    std::vector<Occurrence> found;
    for (const QDomElement& description : descriptions) {
        const QDomNamedNodeMap attributes = description.attributes();
        for (int i = 0; i < attributes.size(); ++i) {
            const QDomAttr attribute = attributes.item(i).toAttr();
            const auto [prefix, local] = splitName(attribute.name());
            if (!prefix.isEmpty() && prefix != "xmlns"_L1 && local == property.local &&
                namespaceOf(description, prefix) == property.ns) {
                found.push_back({.holder = description,
                                 .element = {},
                                 .name = attribute.name(),
                                 .text = attribute.value().trimmed(),
                                 .simple = true});
            }
        }
        for (QDomElement child = description.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            const Resolved name = resolved(child);
            if (name.ns == property.ns && name.local == property.local) {
                found.push_back({.holder = description,
                                 .element = child,
                                 .name = {},
                                 .text = child.text().trimmed(),
                                 .simple = child.firstChildElement().isNull()});
            }
        }
    }
    return found;
}

/// @brief Removes one occurrence of a property from the document.
void remove(const Occurrence& occurrence) {
    QDomElement holder = occurrence.holder;
    if (occurrence.element.isNull()) {
        holder.removeAttribute(occurrence.name);
    } else {
        holder.removeChild(occurrence.element);
    }
}

/// @brief Finds a prefix that means a property's namespace on an element, declaring one if need be.
///
/// A prefix already bound to the namespace in scope, whatever it is, is reused.
QString prefixOn(QDomElement element, const Property& property) {
    for (QDomNode node = element; node.isElement(); node = node.parentNode()) {
        const QDomNamedNodeMap attributes = node.toElement().attributes();
        for (int i = 0; i < attributes.size(); ++i) {
            const QDomAttr attribute = attributes.item(i).toAttr();
            const auto [prefix, local] = splitName(attribute.name());
            // Bound here and not rebound to something else between there and here.
            if (prefix == "xmlns"_L1 && attribute.value() == property.ns &&
                namespaceOf(element, local) == property.ns) {
                return local;
            }
        }
    }
    for (int suffix = 1;; ++suffix) {
        const QString candidate =
            suffix == 1 ? property.prefix : property.prefix + QString::number(suffix);
        if (namespaceOf(element, candidate).isEmpty()) {
            element.setAttribute("xmlns:" + candidate, property.ns);
            return candidate;
        }
    }
}

/// @brief Sets a property to a value, or removes it, leaving the document with one truth.
///
/// An attribute that is there keeps its place and prefix and takes the value.
/// Otherwise the property becomes an attribute of @p home. Every other
/// occurrence, in particular any child-element form, goes.
void setProperty(const std::vector<QDomElement>& descriptions, QDomElement home,
                 const Property& property, const std::optional<QString>& value) {
    const std::vector<Occurrence> found = occurrencesOf(descriptions, property);
    const Occurrence* kept = nullptr;
    if (value) {
        const auto attribute = std::ranges::find_if(
            found, [](const Occurrence& occurrence) { return occurrence.element.isNull(); });
        if (attribute != found.end()) {
            kept = &*attribute;
            QDomElement holder = kept->holder;
            holder.setAttribute(kept->name, *value);
        } else {
            home.setAttribute(prefixOn(home, property) + ':' + property.local, *value);
        }
    }
    for (const Occurrence& occurrence : found) {
        if (&occurrence != kept) {
            remove(occurrence);
        }
    }
}

/// @brief Spells an encoded value as the text of an XMP attribute; nothing for an unset one.
std::optional<QString> spell(const Encoded& encoded) {
    struct Speller {
        std::optional<QString> operator()(std::monostate) const {
            return std::nullopt;
        }
        std::optional<QString> operator()(bool flag) const {
            return flag ? "True"_L1 : "False"_L1;
        }
        std::optional<QString> operator()(double value) const {
            return number(value);
        }
        std::optional<QString> operator()(const std::string& text) const {
            return QString::fromStdString(text);
        }
        std::optional<QString> operator()(const Compound& compound) const {
            // The members in the codec's order: left, top, right, bottom, or the ratio alone.
            QStringList parts;
            for (const auto& [name, value] : compound) {
                parts.push_back(number(value));
            }
            return parts.join(',');
        }
        std::optional<QString> operator()(const PointList& points) const {
            // "x,y;x,y;...", in the curve's order.
            QStringList parts;
            for (const auto& [x, y] : points) {
                parts.push_back(number(x) + ',' + number(y));
            }
            return parts.join(';');
        }
        static QString number(double value) {
            char buffer[64];
            const auto written = std::to_chars(buffer, buffer + sizeof buffer, value);
            return QString::fromLatin1(buffer, written.ptr - buffer);
        }
    };
    return std::visit(Speller{}, encoded);
}

/// @brief Reads a whole text as a finite-or-not double, or nothing when it is not a number.
std::optional<double> numberIn(const QString& text) {
    const std::string trimmed = text.trimmed().toStdString();
    const std::string_view bytes = withoutPlus(trimmed);
    double value = 0.0;
    const auto result = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (bytes.empty() || result.ec != std::errc{} || result.ptr != bytes.data() + bytes.size()) {
        return std::nullopt;
    }
    return value;
}

/// @brief Lists the encoded values an attribute's text could stand for, most likely first.
///
/// The text does not say what it is: `1.5` is a number for a slider and a
/// ratio for a crop, and the codec alone knows which the row wants, so the
/// compound readings are the shapes the row's own encode gives.
std::vector<Encoded> readingsOf(const FieldDescriptor& descriptor, const QString& text) {
    std::vector<Encoded> readings;
    if (text.compare("True"_L1, Qt::CaseInsensitive) == 0) {
        readings.emplace_back(true);
    } else if (text.compare("False"_L1, Qt::CaseInsensitive) == 0) {
        readings.emplace_back(false);
    }
    if (const auto value = numberIn(text)) {
        readings.emplace_back(*value);
    }
    const QStringList parts = text.split(',');
    for (const std::vector<std::string>& names : compoundShapes(descriptor)) {
        if (static_cast<std::size_t>(parts.size()) != names.size()) {
            continue;
        }
        Compound compound;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto value = numberIn(parts[static_cast<qsizetype>(i)]);
            if (!value) {
                break;
            }
            compound.emplace_back(names[i], *value);
        }
        if (compound.size() == names.size()) {
            readings.emplace_back(std::move(compound));
        }
    }
    if (takesPoints(descriptor)) {
        if (auto points = parsePointList(text.toStdString())) {
            readings.emplace_back(std::move(*points));
        }
    }
    readings.emplace_back(text.toStdString());
    return readings;
}

/// @brief Applies an attribute's text to the field a row describes, through the codec.
///
/// The first reading the codec takes wins, with the warnings its decode gave
/// (a clamp); none taking it is the codec's malformed warning.
void applyText(const FieldDescriptor& descriptor, const QString& text, DevelopSettings& settings,
               DiagnosticLog& log, const std::filesystem::path& subject) {
    for (const Encoded& reading : readingsOf(descriptor, text)) {
        DevelopSettings trial = settings;
        CollectedDiagnostics trialLog;
        decode(descriptor, reading, trial, trialLog, subject);
        const bool taken = std::ranges::none_of(trialLog.entries(), [](const Diagnostic& entry) {
            return entry.notice == Notice::SettingMalformed;
        });
        if (taken) {
            settings = trial;
            for (const Diagnostic& entry : trialLog.entries()) {
                log.record(entry);
            }
            return;
        }
    }
    reportMalformed(descriptor, log, subject);
}

/// @brief Reads an integer from a whole text.
std::optional<int> integerIn(const QString& text) {
    const std::string trimmed = text.trimmed().toStdString();
    const std::string_view bytes = withoutPlus(trimmed);
    int value = 0;
    const auto result = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (bytes.empty() || result.ec != std::errc{} || result.ptr != bytes.data() + bytes.size()) {
        return std::nullopt;
    }
    return value;
}

/// @brief Reports a value of a property outside the settings table that cannot be read.
void reportUnreadable(std::string_view key, std::string expected, DiagnosticLog& log,
                      const std::filesystem::path& subject) {
    log.record({.notice = Notice::SettingMalformed,
                .severity = Severity::Warning,
                .subject = subject,
                .values = {std::string(key), std::move(expected)}});
}

/// @brief Namespace of builds before ADR 051, which put the version in the URI.
constexpr auto legacyNamespace = QByteArrayView("http://ns.arraw.org/develop/1.0/");

/// @brief Parses an XMP document.
///
/// The legacy namespace is read as the current one, so a sidecar of an earlier build is read
/// whole and written back under the current URI.
/// @throws std::runtime_error naming @p path if the bytes are not well-formed XML.
QDomDocument parse(const QByteArray& bytes, const std::filesystem::path& path) {
    QDomDocument document;
    const QByteArray current(sidecarNamespace.data(),
                             static_cast<qsizetype>(sidecarNamespace.size()));
    const auto result = document.setContent(QByteArray(bytes).replace(legacyNamespace, current),
                                            QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
    if (!result) {
        throw std::runtime_error("sidecar " + path.string() +
                                 " is not valid XML: " + result.errorMessage.toStdString() +
                                 " at line " + std::to_string(result.errorLine));
    }
    return document;
}

/// @brief Reads a file whole.
/// @throws std::runtime_error if it cannot be read.
QByteArray readBytes(const std::filesystem::path& path) {
    QFile file(qtPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("cannot read sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
    return file.readAll();
}

/// @brief Reads `xmp:CreatorTool`; nothing when it is absent, empty or not simple text.
std::optional<std::string> creatorToolIn(const std::vector<QDomElement>& descriptions) {
    const auto found = occurrencesOf(descriptions, xmpProperty("CreatorTool"_L1));
    if (found.empty() || !found.back().simple || found.back().text.isEmpty()) {
        return std::nullopt;
    }
    return found.back().text.toStdString();
}

/// @brief Reads the version a sidecar declares; nothing when it declares none or one that is
/// unreadable.
std::optional<int> versionIn(const std::vector<QDomElement>& descriptions) {
    const auto found = occurrencesOf(descriptions, arrawProperty("version"));
    if (found.empty() || !found.back().simple) {
        return std::nullopt;
    }
    const auto version = integerIn(found.back().text);
    return version && *version >= 1 ? version : std::nullopt;
}

/// @brief Reports a declared version that is unreadable or newer than this arraw's.
void readVersion(const std::vector<QDomElement>& descriptions, DiagnosticLog& log,
                 const std::filesystem::path& subject) {
    if (occurrencesOf(descriptions, arrawProperty("version")).empty()) {
        return;
    }
    const auto version = versionIn(descriptions);
    if (!version) {
        reportUnreadable("version", "an integer of at least 1", log, subject);
    } else if (*version > sidecarVersion) {
        log.record(
            {.notice = Notice::NewerSettingsVersion,
             .severity = Severity::Warning,
             .subject = subject,
             .values = {static_cast<double>(*version), static_cast<double>(sidecarVersion)}});
    }
}

/// @brief Whether a key is one of the properties that hold the local adjustments, or held their
/// own version before `arraw:version` governed them (ADR 051): read as known, written as nothing.
bool isLocalAdjustmentProperty(std::string_view key) {
    return key == "localAdjustments" || key == "localAdjustmentsVersion" ||
           key == "nextLocalAdjustmentId";
}

/// @brief Whether a key is one of the version-2 properties of snapshots and of the presence of the
/// active state (ADR 056).
bool isSnapshotProperty(std::string_view key) {
    return key == "activeStatePresent" || key == "savedBase" || key == "snapshots" ||
           key == "snapshotNextId" || key == "snapshotRevision";
}

/// @brief Reads `arraw:activeStatePresent`; nothing when it is absent or not `True` or `False`.
std::optional<bool> activeStateMarkerIn(const std::vector<QDomElement>& descriptions) {
    const auto found = occurrencesOf(descriptions, arrawProperty("activeStatePresent"));
    if (found.empty() || !found.back().simple) {
        return std::nullopt;
    }
    if (found.back().text.compare("True"_L1, Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (found.back().text.compare("False"_L1, Qt::CaseInsensitive) == 0) {
        return false;
    }
    return std::nullopt;
}

/// @brief Reads a field's text as every value it could stand for.
FieldValue fieldOfText(const QString& raw) {
    FieldValue field;
    field.text = raw.toStdString();
    const QString trimmed = raw.trimmed();
    field.number = numberIn(trimmed);
    if (trimmed.compare("True"_L1, Qt::CaseInsensitive) == 0) {
        field.flag = true;
    } else if (trimmed.compare("False"_L1, Qt::CaseInsensitive) == 0) {
        field.flag = false;
    }
    return field;
}

/// @brief Finds the `rdf:Seq` a property holds, directly or inside a nested `rdf:Description`.
QDomElement sequenceIn(const QDomElement& property) {
    for (QDomElement child = property.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        if (isNamed(child, rdfNamespace, "Seq"_L1)) {
            return child;
        }
        if (isNamed(child, rdfNamespace, "Description"_L1)) {
            if (const QDomElement inner = sequenceIn(child); !inner.isNull()) {
                return inner;
            }
        }
    }
    return {};
}

/// @brief Reads a brush's `arraw:strokes` property: one base64 string per `rdf:li`.
///
/// An empty property is an empty brush, as XMP tools may drop an empty container.
void gatherStrokes(const QDomElement& property, MaskFields& into) {
    into.strokes.reset();
    into.strokesProblem.clear();
    const QDomElement sequence = sequenceIn(property);
    if (sequence.isNull()) {
        if (property.firstChildElement().isNull() && property.text().trimmed().isEmpty()) {
            into.strokes = std::vector<StrokeFields>{};
        } else {
            into.strokesProblem = "'strokes' is not a sequence of strokes";
        }
        return;
    }
    std::vector<StrokeFields> strokes;
    for (QDomElement item = sequence.firstChildElement(); !item.isNull();
         item = item.nextSiblingElement()) {
        if (!isNamed(item, rdfNamespace, "li"_L1)) {
            continue;
        }
        StrokeFields stroke;
        if (strokes.size() >= maximumStrokesPerMask) {
            // Past the cap a stroke is only counted, so it is not decoded.
            strokes.emplace_back();
            continue;
        }
        try {
            stroke.decoded = strokeFromBase64(item.text().trimmed().toStdString());
        } catch (const std::invalid_argument& error) {
            stroke.problem = error.what();
        }
        strokes.push_back(std::move(stroke));
    }
    into.strokes = std::move(strokes);
}

/// @brief Gathers the fields of a mask from an element, in any of RDF's equivalent spellings:
/// attributes of the element, child elements, and the same inside a nested `rdf:Description`.
void gatherFields(const QDomElement& holder, const QString& arrawNs, MaskFields& into) {
    const auto add = [&](const QString& local, FieldValue value) {
        const std::string key = local.toStdString();
        switch (partOfKey(key)) {
        case FieldPart::Top:
            into.top.emplace_back(key, std::move(value));
            break;
        case FieldPart::Geometry:
            into.geometry.emplace_back(key, std::move(value));
            break;
        case FieldPart::Delta:
            into.deltas.emplace_back(key, std::move(value));
            break;
        }
    };
    const QDomNamedNodeMap attributes = holder.attributes();
    QStringList names;
    for (int i = 0; i < attributes.size(); ++i) {
        names.push_back(attributes.item(i).nodeName());
    }
    names.sort();
    for (const QString& name : names) {
        const auto [prefix, local] = splitName(name);
        if (!prefix.isEmpty() && prefix != "xmlns"_L1 && namespaceOf(holder, prefix) == arrawNs) {
            add(local, fieldOfText(holder.attribute(name)));
        }
    }
    for (QDomElement child = holder.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        const Resolved name = resolved(child);
        if (name.ns == rdfNamespace && name.local == "Description"_L1) {
            gatherFields(child, arrawNs, into);
        } else if (name.ns == arrawNs && name.local == "strokes"_L1) {
            gatherStrokes(child, into);
        } else if (name.ns == arrawNs) {
            if (child.firstChildElement().isNull()) {
                add(name.local, fieldOfText(child.text()));
            } else {
                FieldValue structured;
                structured.structured = true;
                add(name.local, std::move(structured));
            }
        }
    }
}

/// @brief Reads the masks and the counter a description holds, with the version they declare.
///
/// @return Whether the sidecar holds the list in any of its three properties.
bool readLocalAdjustmentsIn(const std::vector<QDomElement>& descriptions, DevelopState& state,
                            DiagnosticLog& log, const std::filesystem::path& subject) {
    const auto lists = occurrencesOf(descriptions, arrawProperty("localAdjustments"));
    const auto counters = occurrencesOf(descriptions, arrawProperty("nextLocalAdjustmentId"));
    if (lists.empty() && counters.empty()) {
        return false;
    }
    std::optional<double> stored;
    if (!counters.empty()) {
        stored = counters.back().simple ? numberIn(counters.back().text) : std::nullopt;
        if (!stored) {
            stored = std::numeric_limits<double>::quiet_NaN();
        }
    }
    std::vector<MaskFields> entries;
    if (!lists.empty()) {
        const QString arrawNs = arrawProperty("").ns;
        const QDomElement property = lists.back().element;
        QDomElement sequence;
        if (!property.isNull()) {
            for (QDomElement child = property.firstChildElement(); !child.isNull();
                 child = child.nextSiblingElement()) {
                if (isNamed(child, rdfNamespace, "Seq"_L1)) {
                    sequence = child;
                    break;
                }
            }
        }
        if (sequence.isNull()) {
            reportUnreadable("localAdjustments", "a sequence of masks", log, subject);
        } else {
            for (QDomElement item = sequence.firstChildElement(); !item.isNull();
                 item = item.nextSiblingElement()) {
                if (isNamed(item, rdfNamespace, "li"_L1)) {
                    MaskFields fields;
                    gatherFields(item, arrawNs, fields);
                    entries.push_back(std::move(fields));
                }
            }
        }
    }
    ReadLocalAdjustments read = readLocalAdjustments(entries, stored, log, subject);
    state.localAdjustments = std::move(read.adjustments);
    state.nextLocalAdjustmentId = read.next;
    return true;
}

/// @brief Keys that together record the lens of a photograph (ADR 050, section 8).
constexpr std::array<std::string_view, 5> lensRecordKeys = {
    "lensMaker", "lensModel", "lensCropFactor", "lensOrigin", "lensDatabase"};

/// @brief Whether a key of the `arraw:` namespace belongs to the negative settings.
bool isNegativeProperty(std::string_view key) {
    return key == "demosaic" || key == "lensDistortion" || key == "lensVignetting" ||
           key == "lensLateralCa" || std::ranges::find(lensRecordKeys, key) != lensRecordKeys.end();
}

/// @brief Reports every `arraw:` property that is not a setting arraw knows.
void readUnknownKeys(const std::vector<QDomElement>& descriptions, DiagnosticLog& log,
                     const std::filesystem::path& subject) {
    const QString ns = arrawProperty("").ns;
    for (const QDomElement& description : descriptions) {
        for (const Resolved& property : propertiesOf(description)) {
            const std::string key = property.local.toStdString();
            if (property.ns == ns && key != "version" && !isNegativeProperty(key) &&
                !isLocalAdjustmentProperty(key) && !isSnapshotProperty(key) &&
                findDescriptor(key) == nullptr) {
                log.record({.notice = Notice::SettingUnknown,
                            .severity = Severity::Warning,
                            .subject = subject,
                            .values = {key}});
            }
        }
    }
}

/// @brief Reads one of the lens switches, `True` or `False`.
///
/// Anything else is reported and read as off: the file keeps it regardless, as a write touches the
/// key only when the setting differs from what this reading gave.
/// @return Whether the key is there.
bool flagIn(const std::vector<QDomElement>& descriptions, std::string_view key, bool& flag,
            DiagnosticLog& log, const std::filesystem::path& subject) {
    const auto found = occurrencesOf(descriptions, arrawProperty(key));
    if (found.empty()) {
        return false;
    }
    const QString text = found.back().text.trimmed();
    if (found.back().simple && text.compare("True"_L1, Qt::CaseInsensitive) == 0) {
        flag = true;
    } else if (found.back().simple && text.compare("False"_L1, Qt::CaseInsensitive) == 0) {
        flag = false;
    } else {
        reportUnreadable(key, "True or False", log, subject);
    }
    return true;
}

/// @brief Reads the vignetting amount: a number from 0 to 150, or `True` or `False` as 100 or 0.
///
/// A number outside the range is clamped and reported with ::arraw::Notice::SettingClamped.
/// Anything else is reported and read as 0: the file keeps it regardless, as a write touches the
/// key only when the setting differs from what this reading gave.
/// @return Whether the key is there.
bool amountIn(const std::vector<QDomElement>& descriptions, std::string_view key, float& amount,
              DiagnosticLog& log, const std::filesystem::path& subject) {
    const auto found = occurrencesOf(descriptions, arrawProperty(key));
    if (found.empty()) {
        return false;
    }
    const QString text = found.back().text.trimmed();
    const std::optional<double> value = found.back().simple ? numberIn(text) : std::nullopt;
    if (found.back().simple && text.compare("True"_L1, Qt::CaseInsensitive) == 0) {
        amount = profileVignettingAmount;
    } else if (found.back().simple && text.compare("False"_L1, Qt::CaseInsensitive) == 0) {
        amount = minimumVignettingAmount;
    } else if (value && std::isfinite(*value)) {
        const double clamped = std::clamp(*value, static_cast<double>(minimumVignettingAmount),
                                          static_cast<double>(maximumVignettingAmount));
        amount = shortestFloat(clamped);
        if (clamped != *value) {
            log.record({.notice = Notice::SettingClamped,
                        .severity = Severity::Warning,
                        .subject = subject,
                        .values = {std::string(key), *value, clamped}});
        }
    } else {
        amount = minimumVignettingAmount;
        reportUnreadable(key, "a number from 0 to 150, or True or False", log, subject);
    }
    return true;
}

/// @brief Reads the recorded lens, which is the keys `lensMaker`, `lensModel`, `lensCropFactor`,
/// `lensOrigin` and `lensDatabase` together.
///
/// A record without a model, or with a crop factor that is not a positive number, cannot name a
/// lens and is reported and read as none; the lens is then matched again from the file. An origin
/// that is not a token is reported and read as `matched`.
/// @return The lens, or nothing when none of the keys is there or the record cannot be used.
std::optional<RecordedLens> recordedLensIn(const std::vector<QDomElement>& descriptions,
                                           DiagnosticLog& log,
                                           const std::filesystem::path& subject) {
    const auto textOf = [&](std::string_view key) -> std::optional<QString> {
        const auto found = occurrencesOf(descriptions, arrawProperty(key));
        if (found.empty()) {
            return std::nullopt;
        }
        return found.back().simple ? std::optional{found.back().text.trimmed()}
                                   : std::optional{QString()};
    };
    const auto maker = textOf("lensMaker");
    const auto model = textOf("lensModel");
    const auto crop = textOf("lensCropFactor");
    const auto origin = textOf("lensOrigin");
    const auto database = textOf("lensDatabase");
    if (!maker && !model && !crop && !origin && !database) {
        return std::nullopt;
    }
    if (!model || model->isEmpty()) {
        reportUnreadable("lensModel", "the model of the recorded lens", log, subject);
        return std::nullopt;
    }
    // A record without its crop factor cannot say what scale its profile was applied at.
    const std::optional<double> cropFactor = crop ? numberIn(*crop) : std::nullopt;
    if (!cropFactor || !std::isfinite(*cropFactor) || *cropFactor <= 0.0) {
        reportUnreadable("lensCropFactor", "a positive number", log, subject);
        return std::nullopt;
    }
    RecordedLens lens;
    lens.maker = maker ? maker->toStdString() : std::string{};
    lens.model = model->toStdString();
    lens.cropFactor = *cropFactor;
    lens.database = database ? database->toStdString() : std::string{};
    if (origin) {
        if (const auto token = parseLensOrigin(origin->toStdString())) {
            lens.origin = *token;
        } else {
            reportUnreadable("lensOrigin", "one of matched chosen", log, subject);
        }
    }
    return lens;
}

/// @brief Reads the negative settings of the photograph: `arraw:demosaic` and the lens keys.
///
/// A demosaic token that is not one of ::arraw::demosaicTokens, or a value that is not simple
/// text, is reported and read as the default (AHD); the lens keys are read as
/// ::arraw::LensSettings describes. The file keeps what it cannot read regardless, as a write
/// touches a key only when the negative differs from what this reading gave (see
/// ::arraw::writeSidecar).
/// @return The settings, or nothing when the sidecar has none of the keys.
std::optional<NegativeSettings> negativeIn(const std::vector<QDomElement>& descriptions,
                                           DiagnosticLog& log,
                                           const std::filesystem::path& subject) {
    NegativeSettings negative;
    bool recorded = false;
    if (const auto found = occurrencesOf(descriptions, arrawProperty("demosaic")); !found.empty()) {
        recorded = true;
        const auto token = found.back().simple
                               ? parseDemosaic(found.back().text.trimmed().toStdString())
                               : std::nullopt;
        if (token) {
            negative.demosaic = *token;
        } else {
            std::string expected = "one of";
            for (const std::string_view name : demosaicTokens) {
                expected += ' ';
                expected += name;
            }
            reportUnreadable("demosaic", std::move(expected), log, subject);
        }
    }
    recorded =
        flagIn(descriptions, "lensDistortion", negative.lens.distortion, log, subject) || recorded;
    recorded =
        amountIn(descriptions, "lensVignetting", negative.lens.vignettingAmount, log, subject) ||
        recorded;
    recorded =
        flagIn(descriptions, "lensLateralCa", negative.lens.lateralCa, log, subject) || recorded;
    const bool recordsLens = std::ranges::any_of(lensRecordKeys, [&](std::string_view key) {
        return !occurrencesOf(descriptions, arrawProperty(key)).empty();
    });
    if (recordsLens) {
        recorded = true;
        negative.lens.lens = recordedLensIn(descriptions, log, subject);
    }
    return recorded ? std::optional{negative} : std::nullopt;
}

/// @brief Reads a whole number, which XMP may spell as a real: `3` and `3.0` alike.
std::optional<int> wholeNumberIn(const QString& text) {
    const auto value = numberIn(text);
    if (!value || !std::isfinite(*value) || std::abs(*value) > 1e9 ||
        *value != std::trunc(*value)) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

/// @brief Reads `xmp:Rating` and `xmp:Label` as marks.
///
/// What cannot become a mark is reported and read as no mark. The file keeps
/// it regardless: a write touches a mark only when it differs from what this
/// reading gave (see ::arraw::writeSidecar).
PhotoMarks marksIn(const std::vector<QDomElement>& descriptions, DiagnosticLog& log,
                   const std::filesystem::path& subject) {
    PhotoMarks marks;
    const auto ratings = occurrencesOf(descriptions, xmpProperty("Rating"_L1));
    if (!ratings.empty()) {
        const auto whole =
            ratings.back().simple ? wholeNumberIn(ratings.back().text) : std::nullopt;
        if (!whole) {
            reportUnreadable("xmp:Rating", "a whole number from -1 to 5", log, subject);
        } else {
            marks.rating = std::clamp(*whole, rejectedRating, highestRating);
            if (marks.rating != *whole) {
                log.record({.notice = Notice::SettingClamped,
                            .severity = Severity::Warning,
                            .subject = subject,
                            .values = {std::string("xmp:Rating"), static_cast<double>(*whole),
                                       static_cast<double>(marks.rating)}});
            }
        }
    }
    const auto labels = occurrencesOf(descriptions, xmpProperty("Label"_L1));
    if (labels.empty() || (labels.back().simple && labels.back().text.isEmpty())) {
        return marks;
    }
    const std::string text = labels.back().text.toStdString();
    const auto label = std::ranges::find_if(
        colorLabelNames, [&](const auto& entry) { return entry.second == text; });
    if (labels.back().simple && label != colorLabelNames.end()) {
        marks.label = label->first;
        return marks;
    }
    std::string expected = "one of ";
    for (const auto& [value, name] : colorLabelNames) {
        expected += (expected.size() > 7 ? ", " : "") + std::string(name);
    }
    reportUnreadable("xmp:Label", std::move(expected), log, subject);
    return marks;
}

/// @brief Says the encoding of a document is UTF-8, which is what it is written in.
///
/// Qt decodes a document by its declaration and always writes UTF-8, so a
/// declaration of another encoding would make the file say it is one thing and be another.
void declareUtf8(QDomDocument& document) {
    const QDomNode first = document.firstChild();
    if (first.isProcessingInstruction() && first.nodeName() == "xml"_L1) {
        QDomProcessingInstruction declaration = first.toProcessingInstruction();
        static const QRegularExpression encoding(R"(encoding\s*=\s*(['"])[^'"]*\1)");
        QString data = declaration.data();
        declaration.setData(data.replace(encoding, "encoding='UTF-8'"_L1));
    }
}

/// @brief Whether a file of another extension shares a photograph's stem.
///
/// Probes the names directly, in the lower and the upper case, so it costs the
/// same in a directory of five files as of fifty thousand.
bool hasSibling(const std::filesystem::path& photo, std::span<const std::string_view> extensions) {
    std::error_code error;
    for (const std::string_view extension : extensions) {
        std::string upper(extension);
        std::ranges::transform(upper, upper.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        for (const std::string& spelling : {std::string(extension), upper}) {
            std::filesystem::path candidate = photo;
            candidate.replace_extension(spelling);
            if (candidate.filename() != photo.filename() &&
                std::filesystem::exists(candidate, error) &&
                !std::filesystem::equivalent(candidate, photo, error)) {
                return true;
            }
        }
    }
    return false;
}

// ---- Snapshots (ADR 056)

/// @brief Reads a whole text of decimal digits as an unsigned number.
std::optional<std::uint64_t> decimalIn(const QString& text) {
    const std::string trimmed = text.trimmed().toStdString();
    if (trimmed.empty() || !std::ranges::all_of(trimmed, [](unsigned char character) {
            return std::isdigit(character) != 0;
        })) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (result.ec != std::errc{} || result.ptr != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return value;
}

/// @brief Spells a time as the ISO 8601 UTC date XMP uses.
QString spellTime(std::chrono::sys_seconds time) {
    return QDateTime::fromSecsSinceEpoch(time.time_since_epoch().count(), QTimeZone::UTC)
        .toString(Qt::ISODate);
}

/// @brief Reads a time; nothing when it is not a date or lies outside what a snapshot may carry.
std::optional<std::chrono::sys_seconds> timeIn(const QString& text) {
    const QDateTime parsed = QDateTime::fromString(text.trimmed(), Qt::ISODate);
    if (!parsed.isValid()) {
        return std::nullopt;
    }
    const std::int64_t seconds = parsed.toSecsSinceEpoch();
    if (seconds < 0 || seconds > 253402300799) {
        return std::nullopt;
    }
    return std::chrono::sys_seconds{std::chrono::seconds{seconds}};
}

/// @brief The text of an occurrence as the file has it, not trimmed.
QString rawText(const Occurrence& occurrence) {
    return occurrence.element.isNull() ? occurrence.holder.attribute(occurrence.name)
                                       : occurrence.element.text();
}

/// @brief Finds the element that holds the fields of a list item: the item, or the
/// `rdf:Description` nested in it by tools that rewrite `rdf:parseType="Resource"`.
QDomElement fieldHolder(const QDomElement& item) {
    const QString ns = arrawProperty("").ns;
    const auto holdsArraw = [&](const QDomElement& element) {
        const auto properties = propertiesOf(element);
        return std::ranges::any_of(properties,
                                   [&](const Resolved& property) { return property.ns == ns; });
    };
    if (holdsArraw(item)) {
        return item;
    }
    for (QDomElement child = item.firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        if (isNamed(child, rdfNamespace, "Description"_L1)) {
            if (const QDomElement inner = fieldHolder(child); holdsArraw(inner)) {
                return inner;
            }
        }
    }
    return item;
}

/// @brief Finds a field of a list item; the last of it when the file repeats it.
std::optional<Occurrence> fieldOf(const QDomElement& holder, std::string_view key) {
    const auto found = occurrencesOf({holder}, arrawProperty(key));
    if (found.empty()) {
        return std::nullopt;
    }
    return found.back();
}

/// @brief Replaces a field of a list item by a child element, or removes it.
///
/// Fields are elements, not attributes, because states and thumbnails are long and an attribute
/// value is normalised by XML readers.
void setField(QDomElement holder, std::string_view key, const std::optional<QString>& value) {
    for (const Occurrence& occurrence : occurrencesOf({holder}, arrawProperty(key))) {
        remove(occurrence);
    }
    if (!value) {
        return;
    }
    const Property property = arrawProperty(key);
    QDomElement field =
        holder.ownerDocument().createElement(prefixOn(holder, property) + ':' + property.local);
    field.appendChild(holder.ownerDocument().createTextNode(*value));
    holder.appendChild(field);
}

/// @brief Where the snapshot list stands in a document.
struct SnapshotList {
    bool present = false;           ///< Whether the file has `arraw:snapshots`.
    bool usable = true;             ///< Whether its container is one the writer can edit in place.
    QDomElement property;           ///< The `arraw:snapshots` element, null when absent.
    QDomElement sequence;           ///< Its `rdf:Seq`, null when there is none yet.
    std::vector<QDomElement> items; ///< The `rdf:li` of the sequence, in order.
};

/// @brief One list item as read.
struct SnapshotNode {
    QDomElement item;        ///< The `rdf:li`.
    QDomElement holder;      ///< Where its fields are.
    std::uint64_t rawId = 0; ///< Its id when readable, even if the node is not usable.
    bool usable = false;     ///< Whether it became a ::arraw::Snapshot.
};

/// @brief What the snapshot properties of a sidecar say.
struct SnapshotRead {
    SnapshotCollection collection;
    std::optional<SnapshotId> savedBase;
    bool damaged = false;
    SnapshotList list;
    std::vector<SnapshotNode> nodes;
};

/// @brief Reports a snapshot problem.
void reportDamage(DiagnosticLog& log, const std::filesystem::path& subject, std::size_t position,
                  std::uint64_t id, std::string why) {
    log.record({.notice = Notice::SnapshotDamaged,
                .severity = Severity::Warning,
                .subject = subject,
                .values = {position == 0 ? std::string() : std::to_string(position),
                           id == 0 ? std::string() : std::to_string(id), std::move(why)}});
}

/// @brief Finds the snapshot list of a document.
SnapshotList locateList(const std::vector<QDomElement>& descriptions) {
    SnapshotList list;
    const auto found = occurrencesOf(descriptions, arrawProperty("snapshots"));
    if (found.empty()) {
        return list;
    }
    list.present = true;
    list.usable = found.size() == 1 && !found.back().element.isNull();
    if (found.back().element.isNull()) {
        return list;
    }
    list.property = found.back().element;
    list.sequence = sequenceIn(list.property);
    if (list.sequence.isNull()) {
        // An empty property is an empty list, as XMP tools may drop an empty container.
        list.usable = list.usable && list.property.firstChildElement().isNull() &&
                      list.property.text().trimmed().isEmpty();
        return list;
    }
    for (QDomElement item = list.sequence.firstChildElement(); !item.isNull();
         item = item.nextSiblingElement()) {
        if (isNamed(item, rdfNamespace, "li"_L1)) {
            list.items.push_back(item);
        }
    }
    return list;
}

/// @brief Reads one node into a snapshot; the reason in @p why when it cannot be.
std::optional<Snapshot> readNode(SnapshotNode& node, std::size_t position, std::string& why,
                                 DiagnosticLog& log, const std::filesystem::path& subject) {
    const QDomElement holder = node.holder;
    Snapshot snapshot;
    const auto id = fieldOf(holder, "id");
    const auto idValue = id && id->simple ? decimalIn(id->text) : std::nullopt;
    if (!idValue || *idValue == 0) {
        why = "its id is not a positive whole number";
        return std::nullopt;
    }
    node.rawId = *idValue;
    if (*idValue == std::numeric_limits<std::uint64_t>::max()) {
        // No counter can lie above it, so no collection holding it could be valid.
        why = "its id is too large";
        return std::nullopt;
    }
    snapshot.id = SnapshotId{*idValue};
    if (const auto parent = fieldOf(holder, "parent")) {
        const auto value = parent->simple ? decimalIn(parent->text) : std::nullopt;
        if (value && *value != 0 && *value != *idValue) {
            snapshot.parent = SnapshotId{*value};
        } else {
            reportDamage(log, subject, position, *idValue,
                         "its parent is unreadable, so it is a root");
        }
    }
    const auto name = fieldOf(holder, "name");
    if (!name || !name->simple) {
        why = "it has no name";
        return std::nullopt;
    }
    snapshot.name = rawText(*name).toStdString();
    const auto created = fieldOf(holder, "created");
    const auto time = created && created->simple ? timeIn(created->text) : std::nullopt;
    if (!time) {
        why = "its time is not a date";
        return std::nullopt;
    }
    snapshot.created = *time;
    const auto state = fieldOf(holder, "state");
    const auto negative = fieldOf(holder, "negative");
    if (!state || !state->simple || !negative || !negative->simple) {
        why = "it lacks its state or its negative";
        return std::nullopt;
    }
    try {
        snapshot.state = applyStateJson(rawText(*state).toStdString(), DevelopState{}, log);
        snapshot.negative = applyNegativeJson(rawText(*negative).toStdString(), {}, log);
        // The name is checked with everything else, but the thumbnail alone may be lost.
        validate(snapshot);
    } catch (const std::exception& error) {
        why = error.what();
        return std::nullopt;
    }
    const auto thumbnail = fieldOf(holder, "thumbnail");
    const auto key = fieldOf(holder, "thumbnailKey");
    if (thumbnail || key) {
        try {
            if (!thumbnail || !key || !thumbnail->simple || !key->simple) {
                throw std::invalid_argument("a thumbnail needs its bytes and its render key");
            }
            const auto bytes = QByteArray::fromBase64Encoding(
                thumbnail->text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!bytes) {
                throw std::invalid_argument("its thumbnail is not base64");
            }
            SnapshotThumbnail decoded;
            decoded.renderKey = rawText(*key).toStdString();
            decoded.jpeg.resize(static_cast<std::size_t>(bytes.decoded.size()));
            std::memcpy(decoded.jpeg.data(), bytes.decoded.constData(), decoded.jpeg.size());
            validate(decoded);
            snapshot.thumbnail = std::move(decoded);
        } catch (const std::exception& error) {
            reportDamage(log, subject, position, *idValue,
                         std::string("its thumbnail was dropped: ") + error.what());
        }
    }
    return snapshot;
}

/// @brief Reads the snapshots, the counters and the saved base, tolerantly.
SnapshotRead readSnapshotsIn(const std::vector<QDomElement>& descriptions, DiagnosticLog& log,
                             const std::filesystem::path& subject) {
    SnapshotRead read;
    read.list = locateList(descriptions);
    std::uint64_t highest = 0;
    if (read.list.present && !read.list.usable) {
        read.damaged = true;
        reportDamage(log, subject, 0, 0,
                     "the list is not a single sequence of snapshots, so none is listed");
    }
    std::set<std::uint64_t> seen;
    // A list that is not one sequence lists nothing, whichever of its occurrences held items.
    const std::size_t items = read.list.usable ? read.list.items.size() : 0;
    for (std::size_t i = 0; i < items; ++i) {
        SnapshotNode node;
        node.item = read.list.items[i];
        node.holder = fieldHolder(node.item);
        std::string why;
        std::optional<Snapshot> snapshot;
        if (read.collection.entries.size() >= maximumSnapshots) {
            why = "the list is past " + std::to_string(maximumSnapshots) + " snapshots";
            if (const auto id = fieldOf(node.holder, "id"); id && id->simple) {
                node.rawId = decimalIn(id->text).value_or(0);
            }
        } else {
            snapshot = readNode(node, i + 1, why, log, subject);
        }
        highest = std::max(highest, node.rawId);
        if (snapshot && !seen.insert(snapshot->id.value).second) {
            why = "its id is used by an earlier snapshot";
            snapshot.reset();
        }
        if (snapshot) {
            node.usable = true;
            read.collection.entries.push_back(std::move(*snapshot));
        } else {
            read.damaged = true;
            reportDamage(log, subject, i + 1, node.rawId,
                         "it cannot be used and was left out: " + why);
        }
        read.nodes.push_back(node);
    }

    // Parents that are not there make roots; a cycle is cut where it closes.
    std::map<std::uint64_t, std::size_t> index;
    for (std::size_t i = 0; i < read.collection.entries.size(); ++i) {
        index[read.collection.entries[i].id.value] = i;
    }
    for (Snapshot& snapshot : read.collection.entries) {
        if (snapshot.parent && !index.contains(snapshot.parent->value)) {
            reportDamage(log, subject, 0, snapshot.id.value,
                         "its parent " + std::to_string(snapshot.parent->value) +
                             " does not exist, so it is a root");
            snapshot.parent.reset();
        }
    }
    std::map<std::uint64_t, int> state; // 1 on the walk being made, 2 finished
    for (const Snapshot& start : read.collection.entries) {
        std::vector<std::uint64_t> walk;
        Snapshot* at = &read.collection.entries[index[start.id.value]];
        while (at != nullptr && state[at->id.value] == 0) {
            state[at->id.value] = 1;
            walk.push_back(at->id.value);
            Snapshot* next =
                at->parent ? &read.collection.entries[index[at->parent->value]] : nullptr;
            if (next != nullptr && state[next->id.value] == 1) {
                reportDamage(log, subject, 0, at->id.value,
                             "its parent " + std::to_string(next->id.value) +
                                 " leads back to it, so it is a root");
                at->parent.reset();
                next = nullptr;
            }
            at = next;
        }
        for (const std::uint64_t id : walk) {
            state[id] = 2;
        }
    }

    std::optional<std::uint64_t> storedNext;
    std::optional<std::uint64_t> storedRevision;
    std::optional<std::uint64_t> storedBase;
    for (const std::string_view key : {"snapshotNextId", "snapshotRevision", "savedBase"}) {
        const auto found = occurrencesOf(descriptions, arrawProperty(key));
        if (found.empty()) {
            continue;
        }
        const auto value = found.back().simple ? decimalIn(found.back().text) : std::nullopt;
        if (!value) {
            reportUnreadable(key, "a whole number", log, subject);
        } else if (key == "snapshotNextId") {
            storedNext = value;
        } else if (key == "snapshotRevision") {
            storedRevision = value;
        } else {
            storedBase = value;
        }
    }
    read.collection.revision = storedRevision.value_or(0);
    const std::uint64_t floor =
        highest == std::numeric_limits<std::uint64_t>::max() ? highest : highest + 1;
    read.collection.nextId = std::max<std::uint64_t>({storedNext.value_or(1), floor, 1});
    if (storedBase && *storedBase != 0) {
        if (index.contains(*storedBase)) {
            read.savedBase = SnapshotId{*storedBase};
        } else {
            reportDamage(log, subject, 0, *storedBase,
                         "the saved base does not exist, so there is none");
        }
    }
    return read;
}

} // namespace

std::optional<std::string_view> arraw::xmpNamespaceOwner(std::string_view uri) {
    const auto found = std::ranges::find(xmpNamespaceOwners, uri, &XmpNamespaceOwner::uri);
    if (found == xmpNamespaceOwners.end()) {
        return std::nullopt;
    }
    return found->name;
}

std::filesystem::path arraw::sidecarPath(const std::filesystem::path& photo) {
    // The RAW of a pair keeps the stem; anything else that shares a stem with
    // another image has to be told apart by its extension.
    // Every RAW LibRaw opens counts (rawimport::openedRawExtensions), or a RAW
    // and its JPEG would share a sidecar; replace_extension adds the dot.
    const auto& raws = rawimport::openedRawExtensions;
    const bool sharesStem = rawimport::hasRawExtension(photo)
                                ? hasSibling(photo, raws)
                                : hasSibling(photo, raws) || hasSibling(photo, otherExtensions);
    std::filesystem::path name = sharesStem ? photo.filename() : photo.stem();
    const std::filesystem::path directory = photo.parent_path();
    std::filesystem::path exact = directory / name;
    exact += ".xmp";
    std::error_code error;
    if (!std::filesystem::exists(exact, error)) {
        std::filesystem::path upper = directory / name;
        upper += ".XMP";
        if (std::filesystem::exists(upper, error)) {
            return upper;
        }
    }
    return exact;
}

std::optional<SidecarContents> arraw::readSidecar(const std::filesystem::path& photo,
                                                  DiagnosticLog& log) {
    const std::filesystem::path path = sidecarPath(photo);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    QDomDocument document = parse(readBytes(path), path);
    const std::vector<QDomElement> descriptions = descriptionsOf(document);

    SidecarContents contents;
    readVersion(descriptions, log, photo);
    const int version = versionIn(descriptions).value_or(1);
    // Settings are applied onto the neutral defaults, never a kind's: a key
    // the file leaves out means what it meant when the file was written.
    DevelopState state;
    const bool declared = !occurrencesOf(descriptions, arrawProperty("version")).empty();
    bool keys = false;
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        const auto found = occurrencesOf(descriptions, arrawProperty(descriptor.key));
        if (found.empty()) {
            continue;
        }
        keys = true;
        if (found.back().simple) {
            applyText(descriptor, found.back().text, state.settings, log, photo);
        } else {
            reportMalformed(descriptor, log, photo);
        }
    }
    keys = readLocalAdjustmentsIn(descriptions, state, log, photo) || keys;
    bool recorded = declared || keys; // version 1: a version alone is an explicit state.
    if (version >= 2) {
        // Version 2 says whether the state is saved; damaged, only the active keys tell (ADR 056).
        if (const auto marker = activeStateMarkerIn(descriptions)) {
            recorded = *marker;
        } else {
            reportUnreadable("activeStatePresent", "True or False", log, photo);
            recorded = keys;
        }
    }
    if (recorded) {
        contents.state = std::move(state);
    }
    SnapshotRead snapshots = readSnapshotsIn(descriptions, log, photo);
    contents.snapshots = std::move(snapshots.collection);
    contents.savedBase = snapshots.savedBase;
    contents.snapshotsDamaged = snapshots.damaged;
    contents.negative = negativeIn(descriptions, log, photo);
    readUnknownKeys(descriptions, log, photo);
    contents.marks = marksIn(descriptions, log, photo);
    contents.creatorTool = creatorToolIn(descriptions);
    contents.others = foreignNamespacesIn(descriptions);
    return contents;
}

namespace {

/// @brief Spells a written value as the text of an XMP field.
QString spellWritten(const WrittenValue& value) {
    struct Speller {
        QString operator()(bool flag) const {
            return flag ? "True"_L1 : "False"_L1;
        }
        QString operator()(double number) const {
            char buffer[64];
            const auto written = std::to_chars(buffer, buffer + sizeof buffer, number);
            return QString::fromLatin1(buffer, written.ptr - buffer);
        }
        QString operator()(const std::string& text) const {
            return QString::fromStdString(text);
        }
    };
    return std::visit(Speller{}, value);
}

/// @brief Writes the local adjustments and their counter, or removes both.
///
/// A state with no masks and a fresh counter leaves no trace, so every written state reads back
/// equal. The list is an `rdf:Seq` of `rdf:li rdf:parseType="Resource"` structures, which is a
/// child element of the description and so not an attribute like the settings.
void setLocalAdjustments(const std::vector<QDomElement>& descriptions, QDomElement home,
                         const DevelopState& state) {
    const bool needed = !state.localAdjustments.empty() || state.nextLocalAdjustmentId.value > 1;
    const Property listProperty = arrawProperty("localAdjustments");
    setProperty(descriptions, home, listProperty, std::nullopt);
    // The list's own version of earlier builds; `arraw:version` governs it now.
    setProperty(descriptions, home, arrawProperty("localAdjustmentsVersion"), std::nullopt);
    setProperty(descriptions, home, arrawProperty("nextLocalAdjustmentId"),
                needed ? std::optional{QString::number(state.nextLocalAdjustmentId.value)}
                       : std::nullopt);
    if (!needed) {
        return;
    }
    QDomDocument document = home.ownerDocument();
    const QString arrawPrefix = prefixOn(home, listProperty);
    const QString rdfPrefix = prefixOn(home, {QString(rdfNamespace), "rdf"_L1, QString()});
    QDomElement list = document.createElement(arrawPrefix + ":localAdjustments");
    QDomElement sequence = document.createElement(rdfPrefix + ":Seq");
    for (const LocalAdjustment& adjustment : state.localAdjustments) {
        QDomElement item = document.createElement(rdfPrefix + ":li");
        item.setAttribute(rdfPrefix + ":parseType", "Resource"_L1);
        const WrittenMask written = writtenForm(adjustment);
        for (const WrittenFields* fields : {&written.top, &written.geometry, &written.deltas}) {
            for (const auto& [key, value] : *fields) {
                QDomElement field =
                    document.createElement(arrawPrefix + ':' + QString::fromStdString(key));
                field.appendChild(document.createTextNode(spellWritten(value)));
                item.appendChild(field);
            }
        }
        if (written.strokes) {
            QDomElement strokes = document.createElement(arrawPrefix + ":strokes");
            QDomElement strokeSequence = document.createElement(rdfPrefix + ":Seq");
            for (const auto& stroke : written.strokes->strokes()) {
                QDomElement entry = document.createElement(rdfPrefix + ":li");
                entry.appendChild(
                    document.createTextNode(QString::fromStdString(strokeBase64(*stroke))));
                strokeSequence.appendChild(entry);
            }
            strokes.appendChild(strokeSequence);
            item.appendChild(strokes);
        }
        sequence.appendChild(item);
    }
    list.appendChild(sequence);
    home.appendChild(list);
}

/// @brief A sidecar open for editing under its lock.
struct Document {
    std::filesystem::path path;            ///< The sidecar file.
    QDomDocument dom;                      ///< Its latest content.
    std::vector<QDomElement> descriptions; ///< Its `rdf:Description`s.
    QDomElement home;                      ///< The one arraw's attributes go on.
};

/// @brief What a sidecar says about whether it records develop settings, before a write.
struct Presence {
    int version = 1;      ///< Version the file declares, 1 when it declares none.
    bool present = false; ///< Whether it records develop settings, by its version's rule.
};

/// @brief Whether the file has any of the active develop properties.
bool hasActiveKeys(const std::vector<QDomElement>& descriptions) {
    const bool setting =
        std::ranges::any_of(developSettingDescriptors, [&](const auto& descriptor) {
            return !occurrencesOf(descriptions, arrawProperty(descriptor.key)).empty();
        });
    return setting || !occurrencesOf(descriptions, arrawProperty("localAdjustments")).empty() ||
           !occurrencesOf(descriptions, arrawProperty("nextLocalAdjustmentId")).empty();
}

/// @brief Tells whether the file records develop settings, by the rule of the version it declares.
Presence presenceBefore(const std::vector<QDomElement>& descriptions) {
    Presence presence;
    presence.version = versionIn(descriptions).value_or(1);
    if (presence.version >= 2) {
        const auto marker = activeStateMarkerIn(descriptions);
        presence.present = marker.value_or(hasActiveKeys(descriptions));
    } else {
        presence.present = !occurrencesOf(descriptions, arrawProperty("version")).empty() ||
                           hasActiveKeys(descriptions);
    }
    return presence;
}

/// @brief Holds a sidecar's lock file, `<sidecar>.lock`, for the life of the object.
class SidecarLock {
public:
    /// @throws SidecarLockTimeout if another writer keeps the lock past the timeout.
    /// @throws std::runtime_error if the lock file cannot be made.
    explicit SidecarLock(const std::filesystem::path& sidecar)
        : file_(qtPath(sidecar) + QStringLiteral(".lock")) {
        file_.setStaleLockTime(detail::sidecarStaleLock);
        if (file_.tryLock(detail::sidecarLockTimeout)) {
            return;
        }
        if (file_.error() == QLockFile::LockFailedError) {
            throw SidecarLockTimeout(sidecar, file_.fileName().toStdString());
        }
        throw std::runtime_error("cannot lock sidecar " + sidecar.string() +
                                 (file_.error() == QLockFile::PermissionError
                                      ? ": no permission to make its lock file"
                                      : ": its lock file cannot be made"));
    }

private:
    QLockFile file_;
};

/// @brief Edits or creates the sidecar of a photograph under its lock.
///
/// Takes the lock, rereads the latest file, lets @p change patch it, stamps version 2 and the
/// presence of the active state, and replaces the file atomically. When @p change returns false
/// nothing is stamped or written.
/// @param photo Path of the photograph.
/// @param activeWrite Whether @p change writes the active develop settings.
/// @param change Edit to make; may throw, in which case the file is left alone.
void updateSidecar(const std::filesystem::path& photo, bool activeWrite,
                   const std::function<bool(Document&)>& change) {
    Document document;
    document.path = sidecarPath(photo);
    const std::filesystem::path& path = document.path;
    const SidecarLock lock(path);
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    document.dom = exists ? parse(readBytes(path), path) : parse(QByteArray(newSidecar), path);
    QDomDocument& dom = document.dom;

    const std::vector<QDomElement> packets = packetsOf(dom);
    if (packets.empty()) {
        throw std::runtime_error("sidecar " + path.string() + " is not XMP, so it was left alone");
    }
    document.descriptions = descriptionsOf(dom);
    std::vector<QDomElement>& descriptions = document.descriptions;
    // Reading clamped its values to this version's ranges and dropped what it did not
    // understand, so writing them back would damage what a newer arraw put there (ADR 051).
    if (const auto version = versionIn(descriptions); version && *version > sidecarVersion) {
        throw std::runtime_error("sidecar " + path.string() + " is version " +
                                 std::to_string(*version) + ", newer than the " +
                                 std::to_string(sidecarVersion) +
                                 " this arraw writes, so it was left alone");
    }
    declareUtf8(dom);
    if (descriptions.empty()) {
        const QString prefix = splitName(packets.front().tagName()).first;
        const QString qualified = prefix.isEmpty() ? QString() : prefix + ':';
        QDomElement created = dom.createElement(qualified + "Description");
        created.setAttribute(qualified + "about", QString());
        QDomElement packet = packets.front();
        packet.appendChild(created);
        descriptions.push_back(created);
    }
    // The description that already holds arraw content, or else the first.
    const QString ns = arrawProperty("").ns;
    const auto holder = std::ranges::find_if(descriptions, [&](const QDomElement& description) {
        const auto properties = propertiesOf(description);
        return std::ranges::any_of(properties,
                                   [&](const Resolved& property) { return property.ns == ns; });
    });
    document.home = holder != descriptions.end() ? *holder : descriptions.front();
    // Declared once here, so that list items below reuse the prefix instead of each declaring it.
    prefixOn(document.home, arrawProperty(""));

    // Presence is the pre-write version's rule, taken before anything is stamped.
    const Presence before = exists ? presenceBefore(descriptions) : Presence{};
    if (!change(document)) {
        return;
    }
    setProperty(descriptions, document.home, arrawProperty("version"),
                QString::number(sidecarVersion));
    if (activeWrite) {
        setProperty(descriptions, document.home, arrawProperty("activeStatePresent"), "True"_L1);
    } else if (before.version < 2) {
        // An upgrade, or a new file: keep what the old rule said. A version 2 file keeps its
        // marker as it is, a damaged one included (ADR 056).
        setProperty(descriptions, document.home, arrawProperty("activeStatePresent"),
                    before.present ? "True"_L1 : "False"_L1);
    }

    QSaveFile file(qtPath(path));
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("cannot write sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
    const QByteArray bytes = dom.toByteArray(-1);
    if (file.write(bytes) != bytes.size()) {
        throw std::runtime_error("cannot write sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
    if (!file.commit()) {
        throw std::runtime_error("cannot write sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
}

/// @brief Writes the active settings, the negative and the marks the caller owns.
void patchActive(Document& document, const DevelopState* state, const NegativeSettings* negative,
                 const MarkChanges* marksToWrite) {
    const std::vector<QDomElement>& descriptions = document.descriptions;
    const QDomElement home = document.home;
    const std::filesystem::path& path = document.path;
    if (state != nullptr) {
        for (const FieldDescriptor& descriptor : developSettingDescriptors) {
            setProperty(descriptions, home, arrawProperty(descriptor.key),
                        spell(encode(descriptor, state->settings)));
        }
        setLocalAdjustments(descriptions, home, *state);
    }
    // The negative follows the rule the marks follow, key by key: a key is written only when its
    // setting differs from what reading the file gives, so a token this arraw does not know
    // survives. The default is no key at all, as it is what an absent key means.
    if (negative != nullptr) {
        const NegativeSettings read =
            negativeIn(descriptions, discardedDiagnostics(), path).value_or(NegativeSettings{});
        if (negative->demosaic != read.demosaic) {
            setProperty(descriptions, home, arrawProperty("demosaic"),
                        negative->demosaic == NegativeSettings{}.demosaic
                            ? std::nullopt
                            : std::optional{QString::fromUtf8(
                                  tokenOf(negative->demosaic).data(),
                                  static_cast<qsizetype>(tokenOf(negative->demosaic).size()))});
        }
        const auto setFlag = [&](std::string_view key, bool flag, bool onFile) {
            if (flag != onFile) {
                setProperty(descriptions, home, arrawProperty(key),
                            flag ? std::optional{"True"_L1} : std::nullopt);
            }
        };
        setFlag("lensDistortion", negative->lens.distortion, read.lens.distortion);
        if (negative->lens.vignettingAmount != read.lens.vignettingAmount) {
            setProperty(descriptions, home, arrawProperty("lensVignetting"),
                        negative->lens.vignettingAmount > 0.0F
                            ? std::optional{QString::fromStdString(
                                  shortestText(shortestDouble(negative->lens.vignettingAmount)))}
                            : std::nullopt);
        }
        setFlag("lensLateralCa", negative->lens.lateralCa, read.lens.lateralCa);
        if (negative->lens.lens != read.lens.lens) {
            const auto& lens = negative->lens.lens;
            const auto setKey = [&](std::string_view key, std::optional<QString> value) {
                setProperty(descriptions, home, arrawProperty(key), value);
            };
            setKey("lensMaker",
                   lens ? std::optional{QString::fromStdString(lens->maker)} : std::nullopt);
            setKey("lensModel",
                   lens ? std::optional{QString::fromStdString(lens->model)} : std::nullopt);
            setKey("lensCropFactor",
                   lens ? std::optional{QString::fromStdString(shortestText(lens->cropFactor))}
                        : std::nullopt);
            setKey("lensOrigin", lens ? std::optional{QString::fromUtf8(
                                            tokenOf(lens->origin).data(),
                                            static_cast<qsizetype>(tokenOf(lens->origin).size()))}
                                      : std::nullopt);
            setKey("lensDatabase",
                   lens ? std::optional{QString::fromStdString(lens->database)} : std::nullopt);
        }
    }
    if (marksToWrite != nullptr) {
        // Marks are standard XMP that other tools write too, and some of what they
        // write is no mark arraw has (a rating of 9, a label named "Rot"). So a mark
        // is written only when it differs from what reading the file gave: an
        // unchanged mark leaves the file's own text alone, whatever it says. A mark the
        // caller does not name is not touched at all.
        const PhotoMarks onFile = marksIn(descriptions, discardedDiagnostics(), path);
        if (marksToWrite->rating && *marksToWrite->rating != onFile.rating) {
            setProperty(descriptions, home, xmpProperty("Rating"_L1),
                        QString::number(*marksToWrite->rating));
        }
        if (marksToWrite->label && *marksToWrite->label != onFile.label) {
            const std::optional<ColorLabel>& wanted = *marksToWrite->label;
            std::optional<QString> label;
            if (wanted) {
                const auto named = std::ranges::find_if(
                    colorLabelNames, [&](const auto& entry) { return entry.first == *wanted; });
                if (named != colorLabelNames.end()) {
                    label = QString::fromUtf8(named->second.data(),
                                              static_cast<qsizetype>(named->second.size()));
                }
            }
            setProperty(descriptions, home, xmpProperty("Label"_L1), label);
        }
    }
}

/// @brief Names every mark of a set.
MarkChanges allOf(const PhotoMarks& marks) {
    return {.rating = marks.rating, .label = marks.label};
}

/// @brief Refuses a rating that is not a rating.
void checkRating(const MarkChanges& marks) {
    if (marks.rating && (*marks.rating < rejectedRating || *marks.rating > highestRating)) {
        throw std::invalid_argument("rating must be from -1 to 5");
    }
}

/// @brief Writes the settings, negative and marks of a photograph, and optionally the saved base.
void writeActive(const Photo& photo, bool setBase, std::optional<SnapshotId> base,
                 const MarkChanges& marks) {
    const DevelopState state = photo.state();
    const NegativeSettings negative = photo.negative();
    updateSidecar(photo.path(), true, [&](Document& document) {
        patchActive(document, &state, &negative, &marks);
        if (setBase) {
            if (base) {
                const SnapshotRead read =
                    readSnapshotsIn(document.descriptions, discardedDiagnostics(), document.path);
                if (read.collection.find(*base) == nullptr) {
                    throw SnapshotBaseMissing(*base, document.path);
                }
            }
            setProperty(document.descriptions, document.home, arrawProperty("savedBase"),
                        base ? std::optional{QString::number(base->value)} : std::nullopt);
        }
        return true;
    });
}

/// @brief Spells a snapshot's thumbnail fields onto a node, or removes them.
void setThumbnailFields(QDomElement holder, const std::optional<SnapshotThumbnail>& thumbnail) {
    if (!thumbnail) {
        setField(holder, "thumbnail", std::nullopt);
        setField(holder, "thumbnailKey", std::nullopt);
        return;
    }
    const QByteArray bytes(reinterpret_cast<const char*>(thumbnail->jpeg.data()),
                           static_cast<qsizetype>(thumbnail->jpeg.size()));
    setField(holder, "thumbnail", QString::fromLatin1(bytes.toBase64()));
    setField(holder, "thumbnailKey", QString::fromStdString(thumbnail->renderKey));
}

/// @brief Counts the bytes of the serialized snapshot list, as UTF-8.
std::size_t serializedBytes(const QDomElement& element) {
    QByteArray bytes;
    QTextStream stream(&bytes);
    stream.setEncoding(QStringConverter::Utf8);
    element.save(stream, -1);
    stream.flush();
    return static_cast<std::size_t>(bytes.size());
}

/// @brief Refuses a snapshot list that serializes past the limit.
void checkSubtreeSize(const SnapshotRead& read) {
    if (!read.list.property.isNull() &&
        serializedBytes(read.list.property) > detail::snapshotSubtreeLimit) {
        throw std::invalid_argument("the snapshots would take more than " +
                                    std::to_string(detail::snapshotSubtreeLimit) +
                                    " bytes of the sidecar");
    }
}

/// @brief Takes a structural edit of the snapshot list through the lock.
///
/// Checks the revision and the list's container under the lock, lets @p apply patch the nodes and
/// returns the id it addressed; then raises the revision and the counter, rereads, and validates
/// what a reader would now get. Content the reader left out stays in the file, as nodes are
/// patched in place; a container that cannot be edited in place refuses the edit.
SnapshotUpdate
editSnapshots(const std::filesystem::path& photo, std::uint64_t expectedRevision,
              const std::function<SnapshotId(Document&, const SnapshotRead&)>& apply) {
    SnapshotUpdate result;
    updateSidecar(photo, false, [&](Document& document) {
        const SnapshotRead read =
            readSnapshotsIn(document.descriptions, discardedDiagnostics(), document.path);
        if (!read.list.usable) {
            throw std::runtime_error("sidecar " + document.path.string() +
                                     " holds a snapshot list that cannot be edited without losing "
                                     "data, so it was left alone");
        }
        if (read.collection.revision != expectedRevision) {
            throw SnapshotRevisionConflict(expectedRevision, read.collection.revision);
        }
        result.id = apply(document, read);
        setProperty(document.descriptions, document.home, arrawProperty("snapshotRevision"),
                    QString::number(read.collection.revision + 1));
        setProperty(document.descriptions, document.home, arrawProperty("snapshotNextId"),
                    QString::number(std::max(read.collection.nextId, result.id.value + 1)));
        SnapshotRead after =
            readSnapshotsIn(document.descriptions, discardedDiagnostics(), document.path);
        validate(after.collection);
        checkSubtreeSize(after);
        result.collection = std::move(after.collection);
        result.savedBase = after.savedBase;
        return true;
    });
    return result;
}

/// @brief Says, for an error message, that the list holds entries that cannot be read.
///
/// Empty when every entry is usable. Such entries stay in the file and count toward the limit;
/// there is no way to discard them yet.
std::string unreadableNote(const SnapshotRead& read) {
    const auto unreadable = static_cast<std::size_t>(
        std::ranges::count_if(read.nodes, [](const SnapshotNode& node) { return !node.usable; }));
    if (unreadable == 0 && !read.damaged) {
        return {};
    }
    return "; the list also holds " + std::to_string(unreadable) +
           " unreadable entries (damaged or written by something else), which are kept as they are "
           "and count toward the limit";
}

/// @brief Finds the node of a usable snapshot.
const SnapshotNode& nodeOf(const SnapshotRead& read, SnapshotId id) {
    const auto found = std::ranges::find_if(read.nodes, [&](const SnapshotNode& node) {
        return node.usable && node.rawId == id.value;
    });
    if (found == read.nodes.end()) {
        throw std::invalid_argument("there is no usable snapshot " + std::to_string(id.value) +
                                    unreadableNote(read));
    }
    return *found;
}

} // namespace

SidecarLockTimeout::SidecarLockTimeout(const std::filesystem::path& sidecar,
                                       const std::string& lockFile)
    : std::runtime_error("sidecar " + sidecar.string() +
                         " is locked by another writer (lock file " + lockFile +
                         "); gave up after " + std::to_string(detail::sidecarLockTimeout) +
                         " ms, so nothing was written") {}

void arraw::writeSidecar(const Photo& photo) {
    writeActive(photo, false, std::nullopt, allOf(photo.marks()));
}

void arraw::writeSidecar(const Photo& photo, std::optional<SnapshotId> savedBase) {
    writeActive(photo, true, savedBase, allOf(photo.marks()));
}

void arraw::writeSidecar(const Photo& photo, std::optional<SnapshotId> savedBase,
                         const MarkChanges& marks) {
    checkRating(marks);
    writeActive(photo, true, savedBase, marks);
}

MarkChanges arraw::changedMarks(const PhotoMarks& from, const PhotoMarks& to) {
    MarkChanges changes;
    if (from.rating != to.rating) {
        changes.rating = to.rating;
    }
    if (from.label != to.label) {
        changes.label = to.label;
    }
    return changes;
}

void arraw::writeSidecarMarks(const std::filesystem::path& photo, const PhotoMarks& marks) {
    writeSidecarMarkChanges(photo, allOf(marks));
}

void arraw::writeSidecarMarkChanges(const std::filesystem::path& photo, const MarkChanges& marks) {
    checkRating(marks);
    if (marks.empty()) {
        return;
    }
    updateSidecar(photo, false, [&](Document& document) {
        patchActive(document, nullptr, nullptr, &marks);
        return true;
    });
}

SnapshotUpdate arraw::addSnapshot(const std::filesystem::path& photo,
                                  std::uint64_t expectedRevision, Snapshot draft) {
    // Checked before the lock; the id is the real one only under it.
    draft.id = SnapshotId{std::numeric_limits<std::uint64_t>::max()};
    validate(draft);
    return editSnapshots(
        photo, expectedRevision, [&](Document& document, const SnapshotRead& read) {
            if (read.list.items.size() >= maximumSnapshots) {
                throw std::invalid_argument("a photograph keeps at most " +
                                            std::to_string(maximumSnapshots) +
                                            " snapshots, and this sidecar's list has " +
                                            std::to_string(read.list.items.size()) + " entries, " +
                                            std::to_string(read.collection.entries.size()) +
                                            " of them usable" + unreadableNote(read));
            }
            if (draft.parent && read.collection.find(*draft.parent) == nullptr) {
                throw std::invalid_argument("the parent " + std::to_string(draft.parent->value) +
                                            " is not a snapshot of this photograph");
            }
            if (read.collection.nextId == std::numeric_limits<std::uint64_t>::max()) {
                throw std::invalid_argument("the sidecar has no snapshot id left to give");
            }
            draft.id = SnapshotId{read.collection.nextId};
            QDomDocument dom = document.home.ownerDocument();
            QDomElement sequence = read.list.sequence;
            if (sequence.isNull()) {
                QDomElement property = read.list.property;
                if (property.isNull()) {
                    const Property listProperty = arrawProperty("snapshots");
                    property =
                        dom.createElement(prefixOn(document.home, listProperty) + ":snapshots");
                    document.home.appendChild(property);
                }
                const QString rdf =
                    prefixOn(document.home, {QString(rdfNamespace), "rdf"_L1, QString()});
                sequence = dom.createElement(rdf + ":Seq");
                property.appendChild(sequence);
            }
            const QString rdf =
                prefixOn(document.home, {QString(rdfNamespace), "rdf"_L1, QString()});
            QDomElement item = dom.createElement(rdf + ":li");
            item.setAttribute(rdf + ":parseType", "Resource"_L1);
            sequence.appendChild(item);
            setField(item, "id", QString::number(draft.id.value));
            setField(item, "parent",
                     draft.parent ? std::optional{QString::number(draft.parent->value)}
                                  : std::nullopt);
            setField(item, "name", QString::fromStdString(draft.name));
            setField(item, "created", spellTime(draft.created));
            setField(item, "state", QString::fromStdString(stateToJson(draft.state)));
            setField(item, "negative", QString::fromStdString(negativeToJson(draft.negative)));
            setThumbnailFields(item, draft.thumbnail);
            return draft.id;
        });
}

SnapshotUpdate arraw::renameSnapshot(const std::filesystem::path& photo,
                                     std::uint64_t expectedRevision, SnapshotId id,
                                     std::string name) {
    return editSnapshots(photo, expectedRevision, [&](Document&, const SnapshotRead& read) {
        const SnapshotNode& node = nodeOf(read, id);
        Snapshot renamed = *read.collection.find(id);
        renamed.name = std::move(name);
        validate(renamed);
        setField(node.holder, "name", QString::fromStdString(renamed.name));
        return id;
    });
}

SnapshotUpdate arraw::deleteSnapshot(const std::filesystem::path& photo,
                                     std::uint64_t expectedRevision, SnapshotId id) {
    return editSnapshots(
        photo, expectedRevision, [&](Document& document, const SnapshotRead& read) {
            const SnapshotNode& node = nodeOf(read, id);
            const std::optional<SnapshotId> parent = read.collection.find(id)->parent;
            const QString parentText = parent ? QString::number(parent->value) : QString();
            // Children, as the reader sees them, take the parent; unusable nodes stay as they are.
            for (const Snapshot& child : read.collection.entries) {
                if (child.parent == id) {
                    setField(nodeOf(read, child.id).holder, "parent",
                             parent ? std::optional{parentText} : std::nullopt);
                }
            }
            if (read.savedBase == id) {
                setProperty(document.descriptions, document.home, arrawProperty("savedBase"),
                            parent ? std::optional{parentText} : std::nullopt);
            }
            QDomElement item = node.item;
            item.parentNode().removeChild(item);
            return id;
        });
}

bool arraw::setSnapshotThumbnail(const std::filesystem::path& photo, SnapshotId id,
                                 SnapshotThumbnail thumbnail, std::string_view expectedKey) {
    validate(thumbnail);
    bool written = false;
    updateSidecar(photo, false, [&](Document& document) {
        const SnapshotRead read =
            readSnapshotsIn(document.descriptions, discardedDiagnostics(), document.path);
        if (!read.list.usable) {
            throw std::runtime_error("sidecar " + document.path.string() +
                                     " holds a snapshot list that cannot be edited without losing "
                                     "data, so it was left alone");
        }
        const Snapshot* snapshot = read.collection.find(id);
        if (snapshot == nullptr || expectedKey != thumbnail.renderKey) {
            return false;
        }
        setThumbnailFields(nodeOf(read, id).holder, std::move(thumbnail));
        checkSubtreeSize(
            readSnapshotsIn(document.descriptions, discardedDiagnostics(), document.path));
        written = true;
        return true;
    });
    return written;
}
