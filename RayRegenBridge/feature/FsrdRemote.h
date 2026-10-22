#pragma once
// FsrdRemote -- lets a tool outside the game drive FSRD tests while it runs. Commands are written to
// RRB_Captures\command.txt (next to OptiScaler.log), one per line; the file is read and deleted, the commands run in
// order on the render thread and each result is appended to RRB_Captures\status.txt.
//
//   say <text>                       message on screen for 8 s (sayfor <seconds> <text> for another duration)
//   wait <seconds>                   pauses the queue
//   waitdump                         pauses the queue until the running capture is written
//   capture <label>                  every stage of the next frame
//   sequence <label> <count> <sec>   every stage of count frames, one every sec seconds
//   burst <label> <count>            count consecutive frames, reduced set (color, composed, MVs, depth, normals)
//   set <Key> <value>                [FSRD] option for this session only (not saved), e.g. set MaxRadiance 50
//   reset <Key> / resetall           back to the value before the first set
//   metrics on|off                   live metrics: one summary line per second in status.txt and the log
#include <string>

class FsrdFrameDump;
class FsrdMetrics;

namespace FsrdRemote
{
// Render thread, once per FSRD frame
void Tick(FsrdFrameDump* dump, FsrdMetrics* metrics);

// Appends a line to status.txt (and the log)
void Status(const std::string& line);

// Overlay (menu thread): current message, empty when none
std::string Message();
} // namespace FsrdRemote
