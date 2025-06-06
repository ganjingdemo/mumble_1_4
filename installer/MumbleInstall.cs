// Copyright 2020-2021 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

using System;
using WixSharp;

// base class with info across installers
public class MumbleInstall : Project {
	public MumbleInstall() {
		var allUsersProp = new Property("ALLUSERS", "1");
		this.Language = "en-US,zh-CN,zh-TW";
		this.MajorUpgradeStrategy = MajorUpgradeStrategy.Default;
		this.MajorUpgradeStrategy.RemoveExistingProductAfter = Step.InstallInitialize;
		this.PreserveTempFiles = true;
		this.BackgroundImage = @"..\dlgbmp.bmp";
		this.BannerImage = @"..\bannrbmp.bmp";
		this.LicenceFile = @"..\..\licenses\Mumble.rtf";
		this.UI = WUI.WixUI_Minimal;
		this.ControlPanelInfo.Comments = "Mumble is a free, open source, low latency, high quality voice chat application.";
		this.ControlPanelInfo.Manufacturer = "Mumble VoIP";
		this.ControlPanelInfo.ProductIcon = @"..\icons\mumble.ico";
		this.ControlPanelInfo.UrlInfoAbout = "https://mumble.info";
		this.Properties = new Property[] { allUsersProp };
	}
}
