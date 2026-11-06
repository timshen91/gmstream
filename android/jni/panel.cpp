#include "panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "log.h"

namespace gm {

static bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	out.clear();
	uint8_t buf[256];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.insert(out.end(), buf, buf + n);
	fclose(f);
	return true;
}

static bool read_u32(const std::string &path, uint32_t &v, int index = 0)
{
	std::vector<uint8_t> b;
	if (!read_file(path, b) || b.size() < size_t(4 * (index + 1)))
		return false;
	const uint8_t *p = &b[size_t(4 * index)];
	v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
	return true;
}

std::string PanelSchedule::summary() const
{
	char s[256];
	snprintf(s, sizeof(s), "%s%s: row r out at P%+.0f+%.3f r us, lit at P+%.0f+%.3f r us; frame due by P%.0f us",
	         panel.empty() ? "panel" : panel.c_str(), from_device_tree ? "" : " (assumed)", -line_ns() / 1e3,
	         line_ns() / 1e3, blank_ns() / 1e3, panel_line_ns() / 1e3, deadline_ns(0) / 1e3);
	return s;
}

PanelSchedule PanelSchedule::load(double hz)
{
	PanelSchedule s; // the Odin 3's (Chipone ICNA3520, 120 Hz)
	s.period_ns = 1e9 / hz;
	const std::string mdp = "/proc/device-tree/soc/qcom,mdss_mdp@ae00000/";
	uint32_t want = 0;
	if (!read_u32("/proc/device-tree/soc/qcom,dsi-display-primary/qcom,dsi-default-panel", want)) {
		LOGI("panel: no device tree to read; using the Odin 3's timing");
		return s;
	}
	DIR *d = opendir(mdp.c_str());
	if (!d)
		return s;
	std::string node;
	while (dirent *e = readdir(d)) {
		if (strncmp(e->d_name, "qcom,mdss_dsi_", 14) != 0)
			continue;
		uint32_t ph = 0;
		if (read_u32(mdp + e->d_name + "/phandle", ph) && ph == want) {
			node = mdp + e->d_name + "/";
			break;
		}
	}
	closedir(d);
	if (node.empty())
		return s;
	std::vector<uint8_t> name;
	if (read_file(node + "qcom,mdss-dsi-panel-name", name))
		s.panel.assign(name.begin(), std::find(name.begin(), name.end(), 0));
	for (int i = 0; i < 8; i++) {
		std::string t = node + "qcom,mdss-dsi-display-timings/timing@" + std::to_string(i) + "/";
		uint32_t fps = 0, h = 0, fp = 0, bp = 0, pw = 0, jn = 2, jd = 1;
		if (!read_u32(t + "qcom,mdss-dsi-panel-framerate", fps))
			break;
		if (int(fps) != int(hz + 0.5))
			continue;
		read_u32(t + "qcom,mdss-dsi-panel-height", h);
		read_u32(t + "qcom,mdss-dsi-v-front-porch", fp);
		read_u32(t + "qcom,mdss-dsi-v-back-porch", bp);
		read_u32(t + "qcom,mdss-dsi-v-pulse-width", pw);
		read_u32(t + "qcom,mdss-dsi-panel-jitter", jn, 0);
		read_u32(t + "qcom,mdss-dsi-panel-jitter", jd, 1);
		uint32_t fixed_us = 0;
		bool has_fixed = read_u32(t + "qcom,mdss-mdp-transfer-time-us", fixed_us);
		// dsi_panel_calc_dsi_transfer_time(), command mode, no fixed bit clock.
		uint32_t frame_us = 1000000u / fps;
		uint32_t threshold = fps >= 120 ? 500 : 0;
		uint32_t min_us = frame_us * jn / (jd * 100) + 40; // jitter + RSCC mode threshold
		uint32_t default_prefill = 40 * fps / 60, actual_prefill = bp + fp + pw;
		uint32_t prefill = std::max(default_prefill, actual_prefill);
		uint32_t vtotal = actual_prefill + h;
		uint32_t prefill_us = uint32_t(uint64_t(frame_us) * prefill / vtotal);
		min_us += prefill_us;
		uint32_t transfer_us = has_fixed && fixed_us ? std::min(fixed_us, frame_us - min_us)
		                       : frame_us - std::max(min_us, threshold);
		s.rows = int(h);
		s.vtotal = int(vtotal);
		s.transfer_ns = transfer_us * 1e3;
		s.prefill_ns = prefill_us * 1e3;
		s.prefill_lines = int(prefill);
		s.from_device_tree = true;
		LOGI("panel %s at %u Hz: %u rows, vtotal %u, transfer %u us, prefill %u lines (%u us)", s.panel.c_str(), fps,
		     h, vtotal, transfer_us, prefill, prefill_us);
		return s;
	}
	return s;
}

} // namespace gm
