#include "Sidecar.h"

#include "RawImport.h"
#include "SettingCodec.h"

#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNamedNodeMap>
#include <QDomNodeList>
#include <QDomProcessingInstruction>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
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

/// @brief Parses an XMP document.
/// @throws std::runtime_error naming @p path if the bytes are not well-formed XML.
QDomDocument parse(const QByteArray& bytes, const std::filesystem::path& path) {
    QDomDocument document;
    const auto result =
        document.setContent(bytes, QDomDocument::ParseOption::PreserveSpacingOnlyNodes);
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

/// @brief Reports every `arraw:` property that is not a setting arraw knows.
void readUnknownKeys(const std::vector<QDomElement>& descriptions, DiagnosticLog& log,
                     const std::filesystem::path& subject) {
    const QString ns = arrawProperty("").ns;
    for (const QDomElement& description : descriptions) {
        for (const Resolved& property : propertiesOf(description)) {
            const std::string key = property.local.toStdString();
            if (property.ns == ns && key != "version" && findDescriptor(key) == nullptr) {
                log.record({.notice = Notice::SettingUnknown,
                            .severity = Severity::Warning,
                            .subject = subject,
                            .values = {key}});
            }
        }
    }
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
    for (const FieldDescriptor& descriptor : developSettingDescriptors) {
        const auto found = occurrencesOf(descriptions, arrawProperty(descriptor.key));
        if (found.empty()) {
            continue;
        }
        if (found.back().simple) {
            applyText(descriptor, found.back().text, contents.state.settings, log, photo);
        } else {
            reportMalformed(descriptor, log, photo);
        }
    }
    readUnknownKeys(descriptions, log, photo);
    contents.marks = marksIn(descriptions, log, photo);
    contents.creatorTool = creatorToolIn(descriptions);
    contents.others = foreignNamespacesIn(descriptions);
    return contents;
}

namespace {

/// @brief Edits or creates the sidecar of a photograph.
/// @param photo Path of the photograph.
/// @param state Develop state to write; null to leave the settings of an
/// existing sidecar alone (a new one gets the defaults).
/// @param marks Marks to write.
void writeSidecarFor(const std::filesystem::path& photo, const DevelopState* state,
                     const PhotoMarks& marks) {
    const std::filesystem::path path = sidecarPath(photo);
    std::error_code error;
    const bool exists = std::filesystem::is_regular_file(path, error);
    QDomDocument document =
        exists ? parse(readBytes(path), path) : parse(QByteArray(newSidecar), path);

    const std::vector<QDomElement> packets = packetsOf(document);
    if (packets.empty()) {
        throw std::runtime_error("sidecar " + path.string() + " is not XMP, so it was left alone");
    }
    std::vector<QDomElement> descriptions = descriptionsOf(document);
    // Its values were clamped to this version's ranges on the way in, so
    // writing them back would damage what a newer arraw put there.
    if (const auto version = versionIn(descriptions); version && *version > sidecarVersion) {
        throw std::runtime_error("sidecar " + path.string() + " is version " +
                                 std::to_string(*version) + ", newer than the " +
                                 std::to_string(sidecarVersion) +
                                 " this arraw writes, so it was left alone");
    }
    declareUtf8(document);
    if (descriptions.empty()) {
        const QString prefix = splitName(packets.front().tagName()).first;
        const QString qualified = prefix.isEmpty() ? QString() : prefix + ':';
        QDomElement created = document.createElement(qualified + "Description");
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
    const QDomElement home = holder != descriptions.end() ? *holder : descriptions.front();

    if (state != nullptr || !exists) {
        const DevelopState written = state != nullptr ? *state : DevelopState{};
        setProperty(descriptions, home, arrawProperty("version"), QString::number(sidecarVersion));
        for (const FieldDescriptor& descriptor : developSettingDescriptors) {
            setProperty(descriptions, home, arrawProperty(descriptor.key),
                        spell(encode(descriptor, written.settings)));
        }
    }
    // Marks are standard XMP that other tools write too, and some of what they
    // write is no mark arraw has (a rating of 9, a label named "Rot"). So a mark
    // is written only when it differs from what reading the file gave: an
    // unchanged mark leaves the file's own text alone, whatever it says.
    const PhotoMarks onFile = marksIn(descriptions, discardedDiagnostics(), path);
    if (marks.rating != onFile.rating) {
        setProperty(descriptions, home, xmpProperty("Rating"_L1), QString::number(marks.rating));
    }
    if (marks.label != onFile.label) {
        std::optional<QString> label;
        if (marks.label) {
            const auto named = std::ranges::find_if(
                colorLabelNames, [&](const auto& entry) { return entry.first == *marks.label; });
            if (named != colorLabelNames.end()) {
                label = QString::fromUtf8(named->second.data(),
                                          static_cast<qsizetype>(named->second.size()));
            }
        }
        setProperty(descriptions, home, xmpProperty("Label"_L1), label);
    }

    QSaveFile file(qtPath(path));
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("cannot write sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
    file.write(document.toByteArray(-1));
    if (!file.commit()) {
        throw std::runtime_error("cannot write sidecar " + path.string() + ": " +
                                 file.errorString().toStdString());
    }
}

} // namespace

void arraw::writeSidecar(const Photo& photo) {
    const DevelopState state = photo.state();
    writeSidecarFor(photo.path(), &state, photo.marks());
}

void arraw::writeSidecarMarks(const std::filesystem::path& photo, const PhotoMarks& marks) {
    if (marks.rating < rejectedRating || marks.rating > highestRating) {
        throw std::invalid_argument("rating must be from -1 to 5");
    }
    writeSidecarFor(photo, nullptr, marks);
}
