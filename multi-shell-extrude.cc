/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2014 Henner Zeller <h.zeller@acm.org>
 * Creative commons BY-SA
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

#include "multi-shell-extrude.h"
#include "printer.h"
#include "config-values.h"

// The total length of distance going through a polygon.
double CalcPolygonLen(const Polygon &polygon) {
  double len = 0;
  const int size = polygon.size();
  for (int i = 1; i < size; ++i) {
    len += distance(polygon[i].x - polygon[i-1].x,
                    polygon[i].y - polygon[i-1].y, 0);
  }
  // Back to the beginning.
  len += distance(polygon[size-1].x - polygon[0].x,
                  polygon[size-1].y - polygon[0].y, 0);
  return len;
}

// Requires: Polygon with centroid on (0,0)
static void CreateExtrusion(const Polygon &extrusion_polygon,
                            Printer *printer,
                            double offset_x, double offset_y,
                            double layer_height, double total_height,
                            double rotation_per_mm,
                            double lock_offset) {
  printer->Comment("Center X=%.1f Y=%.1f\n", offset_x, offset_y);
  const double rotation_per_layer = layer_height * rotation_per_mm * 2 * M_PI;
  bool fan_is_on = false;
  printer->SwitchFan(false);
  double height = 0;
  double angle = 0;
  double run_len = 0;
  const bool do_lock = (lock_offset > 0);
  double polygon_len = 0;
  Polygon p;  // active polygon.
  static const int kLockOverlap = 3;
  enum State {
    START, WIDE_LOCK, NORMAL, NARROW_LOCK
  };
  enum State state = START;
  enum State prev_state;
  for (height = 0, angle=0; height < total_height;
       height += layer_height, angle += rotation_per_layer) {
    prev_state = state;

    // Experimental. Locking screws do have smaller/larger diameter at their
    // ends. This goes through the state transitions.
    // What to print. For locking screw we're very simple: we just offset the
    // polygon, but don't do any transition for now.
    // TODO: re-arrange polygon to start at same angle.
    switch (state) {
    case START:
      if (do_lock) {
        state = WIDE_LOCK;
        p = PolygonOffset(extrusion_polygon, lock_offset);
      } else {
        state = NORMAL;
        p = extrusion_polygon;
      }
      break;

    case WIDE_LOCK:
      if (do_lock && height > kLockOverlap) {
        p = extrusion_polygon;
        state = NORMAL;
      }
      break;

    case NORMAL:
      if (do_lock && height > total_height - kLockOverlap) {
        p = PolygonOffset(extrusion_polygon, -lock_offset);
        state = NARROW_LOCK;
      }
      break;
    case NARROW_LOCK: /* terminal state */ break;
    }

    if (state != prev_state) {
      polygon_len = CalcPolygonLen(p);
      printer->MoveTo(p[0].x + offset_x, p[0].y + offset_y, height);
    }

    for (int i = 0; i < (int) p.size(); ++i) {
      if (i == 0) {
        run_len = 0;
      } else {
        run_len += distance(p[i].x - p[i-1].x, p[i].y - p[i-1].y, 0);
      }
      const double fraction = run_len / polygon_len;
      const double a = angle + fraction * rotation_per_layer;
      const double x = p[i].x * cos(a) - p[i].y * sin(a);
      const double y = p[i].y * cos(a) + p[i].x * sin(a);
      const double z = height + layer_height * fraction;
      if (z < total_height - 0.20 * layer_height) {
        printer->ExtrudeTo(x + offset_x, y + offset_y, z);
      } else {
        // In the last layer, we stop extruding to have a smooth finish.
        printer->MoveTo(x + offset_x, y + offset_y, z);
      }
    }

    if (height > 1.5 && !fan_is_on) {
      printer->SwitchFan(true);  // 1.5mm reached - fan on.
      fan_is_on = true;
    }
  }
}

// Read very simple polygon from file: essentially a sequence of x y
// coordinates.
Polygon ReadPolygon(const std::string &filename, double factor) {
  Polygon polygon;
  FILE *in = fopen(filename.c_str(), "r");
  if (!in) {
    fprintf(stderr, "Can't open %s\n", filename.c_str());
    return polygon;
  }
  char buffer[256];
  int line = 0;
  while (fgets(buffer, sizeof(buffer), in)) {
    ++line;
    const char *start = buffer;
    while (*start && isspace(*start))
      start++;
    if (*start == '\0' || *start == '#')
      continue;
    Point2D p;
    if (sscanf(start, "%lf %lf", &p.x, &p.y) == 2) {
      p.x *= factor;
      p.y *= factor;
      polygon.push_back(p);
    } else {
      for (char *end = buffer + strlen(buffer) - 1; isspace(*end); end--) {
        *end = '\0';
      }
      fprintf(stderr, "%s:%d not a comment and not coordinates: '%s'\n",
              filename.c_str(), line, start);
    }
  }
  fclose(in);
  return polygon;
}

// Pump a polygon as if it was not arranged a dot but a circle of radius pump_r
Polygon RadialPumpPolygon(const Polygon& polygon, double pump_r) {
  if (pump_r <= 0)
    return polygon;
  Polygon result;
  for (std::size_t i = 0; i < polygon.size(); ++i) {
    const Point2D &p = polygon[i];
    double from_center = distance(p.x, p.y, 0);
    double stretch = (from_center + pump_r) / from_center;
    result.push_back(Point2D(p.x * stretch, p.y * stretch));
  }
  return result;
}

// Determine radius of circumscribed circle
double GetRadius(const Polygon &polygon) {
  double dist = -1;
  for (size_t i = 0; i < polygon.size(); ++i) {
    dist = std::max(dist, distance(polygon[i].x, polygon[i].y, 0));
  }
  return dist;
}

int main(int argc, char *argv[]) {
  double start_x = 5;  // Initial edge offset.
  double start_y = 5;
  // Some useful default values.
  double nozzle_radius = 0.4 / 2;
  double filament_radius = 1.75 / 2;
  int faces = 720;
  double shell_thickness_factor = 1.9;  // ~2*nozzzle = ~0.8mm shell thickness.

  ParamHeadline h1("Screw-data from template");
  StringParam fun_init    ("AABBBAABBBAABBB", "screw-template", 't', "Template string for screw.");
  FloatParam thread_depth (-1, "thread-depth", 'd',   "Depth of thread, initial-size/5 if negative");
  FloatParam twist        (0.0, "twist",        0,    "Twist ratio of angle per radius fraction (good -0.3..0.3)");

  ParamHeadline h2("Screw-data from polygon file");
  StringParam polygon_file("", "polygon-file", 'D',  "File describing polygon. Files with x y pairs");

  ParamHeadline h3("General Parameters");
  FloatParam total_height (-1,    "height", 'h', "Total height to be printed");
  FloatParam pitch        (30.0,  "pitch",  'p', "Millimeter height a full turn takes. "
                           "Negative for left-turning screw; 0 for straight hull.");
  FloatParam initial_size (10.0, "size",    's', "Polygon sizing parameter. Means radius if from "
                           "--screw-template, factor for --polygon-file");
  FloatParam pump         (0.0,   "pump",    0, "Pump polygon as if the center was not a dot, but a circle of this radius");
  IntParam screw_count    (2,     "number", 'n', "Number of screws to be printed");
  FloatParam initial_shell(0,     "start-offset", 0, "Initial offset for first polygon");
  FloatParam shell_increment(1.2, "offset", 'R', "Offset increment between screws - the clearance");
  FloatParam layer_height (0.16,  "layer-height", 'l', "Height of each layer");
  FloatParam feed_mm_per_sec(100, "feed-rate",    'f', "maximum, in mm/s");
  FloatParam min_layer_time(8,    "layer-time",   'T', "Min time per layer; upper bound for feed-rate");
  FloatParam lock_offset  (-1,    "lock-offset", 0, "EXPERIMENTAL offset to stop screw at end; (radius_increment - 0.8)/2 + 0.05");
  FloatPairParam machine_limit(std::make_pair(150.0f,150.0f), "bed-size",    'L',  "x/y size limit of your printbed.");
  FloatPairParam head_offset(std::make_pair(45.0f,45.0f),     "head-offset", 'o', "dx/dy offset per print.");

  // Output options
  ParamHeadline h4("Output Options");
  BoolParam do_postscript(false, "postscript", 'P', "PostScript output instead of GCode output");
  BoolParam matryoshka(false,    "nested",      0, "For PostScript: show nested (Matryoshka doll style)");

  if (!SetParametersFromCommandline(argc, argv)) {
    return ParameterUsage(argv[0]);
  }

  if (total_height < 0) {
    fprintf(stderr, "\n--height needs to be set\n\n");
    return ParameterUsage(argv[0]);
  }

  if (thread_depth < 0)
    thread_depth = initial_size / 5;

  if (matryoshka && !do_postscript) {
    fprintf(stderr, "Matryoshka mode only valid with postscript\n");
    return ParameterUsage(argv[0]);
  }

  double machine_limit_x = machine_limit.get().first;
  double machine_limit_y = machine_limit.get().second;
  double head_offset_x = head_offset.get().first;
  double head_offset_y = head_offset.get().second;

  matryoshka = matryoshka & do_postscript;   // Formulate it this way.

  // Get polygon we'll be working on. Add pump if needed.
  const Polygon base_polygon =
    RadialPumpPolygon(polygon_file.get().empty()
                      ? RotationalPolygon(fun_init.get().c_str(), initial_size,
                                          thread_depth, twist)
                      : ReadPolygon(polygon_file, initial_size),
                      pump);

  if (base_polygon.empty()) {
    fprintf(stderr, "Polygon empty\n");
    return 1;
  }

  if (matryoshka) {
    Polygon biggst_polygon
      = PolygonOffset(base_polygon,
                      initial_shell + (screw_count-1) * shell_increment);
    double max_radius = GetRadius(biggst_polygon);
    machine_limit_x = 2 * (max_radius + 5);
    machine_limit_y = 2 * (max_radius + 5);
    start_x = max_radius + 5;
    start_y = max_radius + 5;
  }

  const double filament_extrusion_factor = shell_thickness_factor *
    (nozzle_radius * (layer_height/2)) / (filament_radius*filament_radius);

  Printer *printer = NULL;
  if (do_postscript) {
    total_height = std::min(total_height.get(),
                            3 * layer_height); // not needed more.
    // no move lines w/ Matryoshka
    printer = CreatePostscriptPrinter(!matryoshka,
                                      shell_thickness_factor * 2*nozzle_radius);
  } else {
    printer = CreateGCodePrinter(filament_extrusion_factor);
  }
  printer->Preamble(machine_limit_x, machine_limit_y, feed_mm_per_sec);

  printer->Comment("https://github.com/hzeller/gcode-multi-shell-extrude\n");
  printer->Comment("\n");
  printer->Comment(" ");
  for (int i = 0; i < argc; ++i)
    printf("%s ", argv[i]);
  printf("\n");
  printer->Comment("\n");
  if (!polygon_file.get().empty()) {
    printer->Comment("Polygon from polygon-file '%s'\n",
                     polygon_file.get().c_str());
  } else {
    printer->Comment("Polygon from screw template '%s'\n",
                     fun_init.get().c_str());
  }
  printer->Comment("size=%.1fmm h=%.1fmm n=%d (shell-increment=%.1fmm)\n",
                   initial_size.get(), total_height.get(),
                   screw_count.get(), shell_increment.get());
  printer->Comment("thread-depth=%.1fmm faces=%d\n",
                   thread_depth.get(), faces);
  printer->Comment("feed=%.1fmm/s (maximum; layer time at least %.1f s)\n",
                   feed_mm_per_sec.get(), min_layer_time.get());
  printer->Comment("pitch=%.1fmm/turn layer-height=%.3f\n",
                   pitch.get(), layer_height.get());
  printer->Comment("machine limits: bed: (%.0f/%.0f):  "
                  "head-offset: (%.0f,%.0f)\n",
                  machine_limit_x, machine_limit_y,
                  head_offset_x, head_offset_y);
  printer->Comment("----\n");

  printer->Init(machine_limit_x, machine_limit_y, feed_mm_per_sec);

  // How much the whole system should rotate per mm height.
  const double rotation_per_mm = (fabs(pitch) < 0.1) ? 0 : 1.0 / pitch;

  double total_time = 0;
  double total_travel = 0;

  double x = start_x;
  double y = start_y;
  printer->SetSpeed(feed_mm_per_sec);  // initial speed.
  for (int i = 0; i < screw_count; ++i) {
    Polygon polygon = PolygonOffset(base_polygon,
                                    initial_shell + i * shell_increment);
    double radius = GetRadius(polygon);
    if (!matryoshka) {
      // New center.
      x += radius;
      y += radius;
    }
    if (x + radius + 5 > machine_limit_x || y + radius + 5 > machine_limit_y) {
      fprintf(stderr, "With currently configured bedsize and printhead-offset, "
              "only %d screws fit (radius is %.1fmm)\n"
              "Configure your machine constraints with -L <x/y> -o < dx,dy> "
              "(currently -L %.0f,%.0f -o %.0f,%.0f)\n", i, radius,
              machine_limit_x, machine_limit_y, head_offset_x, head_offset_y);
      break;
    }
    printer->MoveTo(x, y, i > 0 ? total_height + 5 : 5);
    float layer_feedrate = CalcPolygonLen(polygon) / min_layer_time;
    layer_feedrate = std::min(layer_feedrate, feed_mm_per_sec.get());
    printer->ResetExtrude();
    printer->SetSpeed(layer_feedrate);
    printer->Comment("Screw #%d, polygon-offset=%.1f\n",
                     i+1, initial_shell + i * shell_increment);
    CreateExtrusion(polygon, printer, x, y, layer_height, total_height,
                    rotation_per_mm, lock_offset);
    const double travel = printer->GetExtrusionDistance();  // since last reset.
    total_travel += travel;
    total_time += travel / layer_feedrate;  // roughly (without acceleration)
    printer->SetSpeed(feed_mm_per_sec);
    printer->Retract();
    printer->GoZPos(total_height + 5);
    if (!matryoshka) {
      x += head_offset_x + radius;
      y += head_offset_y + radius;
    }
  }

  printer->Postamble();
  if (total_time > 0) {  // doesn't make sense to print for PostScript
    fprintf(stderr, "Total time >= %.0f seconds; %.2fm filament\n",
            total_time, total_travel * filament_extrusion_factor / 1000);
  }
}
