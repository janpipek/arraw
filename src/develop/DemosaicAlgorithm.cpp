#include <QString>
#include "develop/DemosaicAlgorithm.h"

int librawUserQual(DemosaicAlgorithm algo) {
    switch (algo) {
    case DemosaicAlgorithm::Linear:
        return 0;
    case DemosaicAlgorithm::VNG:
        return 1;
    case DemosaicAlgorithm::PPG:
        return 2;
    case DemosaicAlgorithm::AHD:
        return 3;
    case DemosaicAlgorithm::DCB:
        return 4;
    case DemosaicAlgorithm::DHT:
        return 11;
    case DemosaicAlgorithm::AAHD:
        return 12;
    }
    return librawUserQual(kDefaultDemosaic);
}

QString demosaicToken(DemosaicAlgorithm algo) {
    switch (algo) {
    case DemosaicAlgorithm::AHD:
        return QStringLiteral("AHD");
    case DemosaicAlgorithm::VNG:
        return QStringLiteral("VNG");
    case DemosaicAlgorithm::PPG:
        return QStringLiteral("PPG");
    case DemosaicAlgorithm::DCB:
        return QStringLiteral("DCB");
    case DemosaicAlgorithm::DHT:
        return QStringLiteral("DHT");
    case DemosaicAlgorithm::AAHD:
        return QStringLiteral("AAHD");
    case DemosaicAlgorithm::Linear:
        return QStringLiteral("Linear");
    }
    return demosaicToken(kDefaultDemosaic);
}

bool sensorSupportsDemosaicSelection(unsigned filters) {
    // 0 = no mosaic (Foveon / linear / already-demosaiced / monochrome);
    // 9 = X-Trans (libraw reinterprets user_qual as Markesteijn). Everything
    // else is a Bayer CFA the seven algorithms genuinely apply to.
    return filters != 0 && filters != 9;
}

DemosaicAlgorithm demosaicFromToken(const QString& token) {
    for (const DemosaicAlgorithm algo : {DemosaicAlgorithm::AHD, DemosaicAlgorithm::VNG,
                                         DemosaicAlgorithm::PPG, DemosaicAlgorithm::DCB,
                                         DemosaicAlgorithm::DHT, DemosaicAlgorithm::AAHD,
                                         DemosaicAlgorithm::Linear}) {
        if (token == demosaicToken(algo)) {
            return algo;
        }
    }
    return kDefaultDemosaic;
}
