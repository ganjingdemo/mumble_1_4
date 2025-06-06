// Copyright 2010-2021 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "Version.h"

#include <QObject>
#include <QRegExp>

namespace Version {

QString getRelease() {
	return MUMBLE_RELEASE;
}

Version::full_t fromString(const QString &version) {
	Version::component_t major, minor, patch;

	if (Version::getComponents(major, minor, patch, version)) {
		return Version::fromComponents(major, minor, patch);
	}

	return Version::UNKNOWN;
}

Version::full_t fromConfig(const QVariant &config) {
	Version::full_t version;

	bool ok                    = false;
	std::uint64_t integerValue = config.toULongLong(&ok);
	if (ok) {
		if ((integerValue >> Version::OFFSET_MAJOR) != 0) {
			// We assume this must be the new version format (v2), as a bit
			// after the 32nd is set.
			version = static_cast< Version::full_t >(integerValue);
		} else {
			version = Version::fromLegacyVersion(static_cast< std::uint32_t >(integerValue));
		}
	} else {
		// The config contains non-numeric characters -> We assume it contains a version string such as "1.5.0".
		// If this call fails, UNKNOWN is returned.
		version = Version::fromString(config.toString());
	}

	if (version == 0) {
		// 0 is not a valid value for a suggested version
		version = Version::UNKNOWN;
	}

	return version;
}

QString toString(Version::full_t v) {
	if (v == Version::UNKNOWN) {
		return QObject::tr("Unknown Version");
	}
	Version::component_t major, minor, patch;
	Version::getComponents(major, minor, patch, v);
	return QString::fromLatin1("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

QString toConfigString(Version::full_t v) {
	if (v == Version::UNKNOWN) {
		return QString();
	}
	return Version::toString(v);
}

bool getComponents(Version::component_t &major, Version::component_t &minor, Version::component_t &patch,
				   const QString &version) {
	QRegExp rx(QLatin1String("(\\d+)\\.(\\d+)\\.(\\d+)(?:\\.(\\d+))?"));

	if (rx.exactMatch(version)) {
		major = rx.cap(1).toInt();
		minor = rx.cap(2).toInt();
		patch = rx.cap(3).toInt();

		return true;
	}
	return false;
}

unsigned int toRaw(int major, int minor, int patch) {
	return (major << 16) | (minor << 8) | patch;
}

void fromRaw(unsigned int version, int *major, int *minor, int *patch) {
	*major = (version & 0xFFFF0000) >> 16;
	*minor = (version & 0xFF00) >> 8;
	*patch = (version & 0xFF);
}

}; // namespace Version
