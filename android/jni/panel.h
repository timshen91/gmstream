// When the panel reads each row of the buffer on screen: Qualcomm's command-
// mode DSI path (display-drivers msm/sde + msm/dsi), worked out from the
// driver's own formulas and the panel's device-tree timing, and checked
// against the fences (the release fence lands where the transfer is computed
// to end, 0.62-0.63 ms before the next present, every frame).
//
// Per refresh, with P the present fence's time (the write-pointer interrupt,
// which the tear check fires at row 1):
//   - the display engine prefetches the first rows from P - prefill;
//   - row r (in the panel's own order: the buffer's rows when it is drawn in
//     the panel's orientation) leaves for the panel at P + (r - 1) * line;
//   - the panel lights row r at P + blank + r * panel_line (its own scan,
//     behind the transfer and falling further behind it);
//   - the transfer ends at P + transfer.
// A row must be written before it is fetched: row r by
//   P - prefill + max(0, r - prefill_lines) * line.

#pragma once

#include <cstdint>
#include <string>

namespace gm {

struct PanelSchedule {
	bool from_device_tree = false;
	std::string panel;      // the device tree's panel name
	int rows = 1920;        // active rows, in the panel's order
	int vtotal = 1977;      // the panel's lines per refresh
	double period_ns = 8333333; // nominal; the lock measures the real one
	double transfer_ns = 7707000;
	double prefill_ns = 337000;
	int prefill_lines = 80;
	double line_ns() const { return transfer_ns / rows; }
	double panel_line_ns() const { return period_ns / vtotal; }
	double blank_ns() const { return (vtotal - rows) * panel_line_ns(); }
	// The latest a row (or, for row 0, the whole frame) may be written.
	double deadline_ns(int row) const
	{
		int r = row > prefill_lines ? row - prefill_lines : 0;
		return -prefill_ns + r * line_ns();
	}
	double lit_ns(int row) const { return blank_ns() + row * panel_line_ns(); }
	std::string summary() const;

	// From /proc/device-tree (the primary DSI display's panel, the timing
	// whose frame rate is `hz`), with the driver's formulas; the Odin 3's
	// values if it can't be read.
	static PanelSchedule load(double hz);
};

} // namespace gm
