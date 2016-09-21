/*
 * Copyright (C) IMAV 2016
 *
 * This file is part of paparazzi
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file "modules/computer_vision/marker/detector.c"
 */

#include <stdio.h>

#include "state.h"
#include "math/pprz_orientation_conversion.h"

#include "modules/computer_vision/cv.h"
#include "modules/computer_vision/marker/detector.h"

#include "modules/pose_history/pose_history.h"
#include "modules/sonar/sonar_bebop.h"

#include "modules/computer_vision/blob/blob_finder.h"

#include "modules/computer_vision/opencv_imav_landingpad.h"     // OpenCV contour based marker detection
#include "subsystems/datalink/telemetry.h"

static bool SHOW_MARKER = true;
static float MARKER_FOUND_TIME_MAX = 5.0;
static int MARKER_WINDOW = 15;

// General outputs
struct Marker marker;

// Helipad detection
static struct video_listener* helipad_listener;
static struct video_listener* blob_listener;


static void geo_locate_marker(struct image_t* img) {
    // Obtain the relative pixel location (measured from center in body frame) rotated to vehicle reference frame
    struct FloatVect3 pixel_relative;
    pixel_relative.x = (float)(img->h / 2) - (float)marker.pixel.y;
    pixel_relative.y = (float)marker.pixel.x - (float)(img->w / 2);
    pixel_relative.z = 400.; // estimated focal length in px

    // Get the rotation measured at image capture
    struct pose_t pose = get_rotation_at_timestamp(img->pprz_ts);

    // Create a orientation representation
    struct FloatRMat ned_to_body;
    float_rmat_of_eulers(&ned_to_body, &pose.eulers);

    // Rotate the pixel vector from body frame to the north-east-down frame
    struct FloatVect3 geo_relative;
    float_rmat_transp_vmult(&geo_relative, &ned_to_body, &pixel_relative);

    // Divide by z-component to normalize the projection vector
    float zi = geo_relative.z;

    // Pointing up or horizontal -> no ground projection
    if (zi <= 0.) { return; }

    // Scale the parameters based on distance to ground and focal point
    struct NedCoor_f *pos = stateGetPositionNed_f();
    float agl = sonar_bebop.distance; // -pos->z

    geo_relative.x *= agl/zi;
    geo_relative.y *= agl/zi;
    geo_relative.z = agl;

    // TODO filter this location over time to reduce the jitter in output
    // TODO use difference in position as a velocity estimate along side opticflow in hff...

    // NED
    marker.geo_location.x = pos->x + geo_relative.x;
    marker.geo_location.y = pos->y + geo_relative.y;
    marker.geo_location.z = 0;

    //  OLD METHOD
//    struct camera_frame_t cam;
//    cam.px = marker.pixel.x;
//    cam.py = marker.pixel.y;
//    cam.f = 400; // Focal length [px]
//    cam.h = img->w; // Frame height [px]
//    cam.w = img->h; // Frame width [px]
//
//    georeference_project_target(&cam);
//    marker.geo_location.x = POS_FLOAT_OF_BFP(geo.target_abs.x);
//    marker.geo_location.y = POS_FLOAT_OF_BFP(geo.target_abs.y);
//    marker.geo_location.z = POS_FLOAT_OF_BFP(geo.target_abs.z);
    //fprintf(stderr, "[marker] found! %.3f, %.3f, %.3f, %.3f\n", geo_relative.x, geo_relative.y, marker.geo_location.x, marker.geo_location.y);
}


static void marker_detected(struct image_t* img, int pixelx, int pixely)
{
    marker.detected = true;

    // store marker pixel location
    marker.pixel.x = pixelx;
    marker.pixel.y = pixely;

    // calculate geo location
    geo_locate_marker(img);

    // Increase marker detected time and mid counter

    marker.found_time += img->dt / 1000000.f;

    if (marker.found_time > MARKER_FOUND_TIME_MAX) { marker.found_time = MARKER_FOUND_TIME_MAX; }


//    if (marker.pixel.x < 120 + MARKER_WINDOW &&
//        marker.pixel.x > 120 - MARKER_WINDOW &&
//        marker.pixel.y < 120 + MARKER_WINDOW &&
//        marker.pixel.y > 120 - MARKER_WINDOW)
//    {
//        marker.mid = true;
//    } else {
//        marker.mid = false;
//    }

}

static void marker_not_detected(struct image_t* img)
{
    marker.detected = false;
//    marker.mid = false;

    marker.found_time -= 2 * img->dt / 1000000.f;
    if (marker.found_time < 0) { marker.found_time = 0; }
}

static struct image_t *detect_colored_blob(struct image_t* img) {

    // Color Filter
    struct image_filter_t filter;
    filter.y_min = 0;    // red
    filter.y_max = 110;
    filter.u_min = 52;
    filter.u_max = 140;
    filter.v_min = 140;
    filter.v_max = 255;

    int threshold = 50;

    // Output image
    struct image_t dst;
    image_create(&dst, img->w, img->h, IMAGE_GRADIENT);

    // Labels
    uint16_t labels_count = 512;
    struct image_label_t labels[512];

    // Blob finder
    image_labeling(img, &dst, &filter, 1, labels, &labels_count);

    int largest_id = -1;
    int largest_size = 0;

    // Find largest
    for (int i=0; i<labels_count; i++) {
        // Only consider large blobs
        if (labels[i].pixel_cnt > threshold) {
            if (labels[i].pixel_cnt > largest_size) {
                largest_size = labels[i].pixel_cnt;
                largest_id = i;
            }
        }
    }

    if (largest_id >= 0)
    {
        int xloc   = labels[largest_id].x_sum / labels[largest_id].pixel_cnt * 2;
        int yloc   = labels[largest_id].y_sum / labels[largest_id].pixel_cnt;
        marker_detected(img, xloc, yloc);
    }
    else
    {
        marker_not_detected(img);
    }

    image_free(&dst);

//    fprintf(stderr, "[blob] fps %.2f \n", (1000000.f / img->dt));

    return NULL;
}

static struct image_t *detect_helipad_marker(struct image_t* img)
{
    struct results helipad_marker = opencv_imav_landing(
            (char*) img->buf,
            img->w,
            img->h,
            2, //squares
            210, //binary threshold
            1, img->dt); //modify image

    if (helipad_marker.marker)
    {
        marker_detected(img, helipad_marker.maxx, helipad_marker.maxy);
    }
    else
    {
        marker_not_detected(img);
    }
    return NULL;
}

static struct image_t *draw_target_marker(struct image_t* img)
{
    if (marker.detected && SHOW_MARKER) {
        struct point_t t = {marker.pixel.x, marker.pixel.y - 50},
                b = {marker.pixel.x, marker.pixel.y + 50},
                l = {marker.pixel.x - 50, marker.pixel.y},
                r = {marker.pixel.x + 50, marker.pixel.y};

        image_draw_line(img, &t, &b);
        image_draw_line(img, &l, &r);
    }

    DOWNLINK_SEND_DETECTOR(DefaultChannel, DefaultDevice,
                           &marker.detected,
                           &marker.pixel.x,
                           &marker.pixel.y,
                           &marker.found_time);

    return img;
}


void detector_init(void)
{
    // Add detection function to CV
    helipad_listener = cv_add_to_device_async(&DETECTOR_CAMERA1, detect_helipad_marker, 5);
    helipad_listener->maximum_fps = 20;
//    helipad_listener = cv_add_to_device(&DETECTOR_CAMERA1, detect_helipad_marker);

    blob_listener = cv_add_to_device(&DETECTOR_CAMERA1, detect_colored_blob);

    cv_add_to_device(&DETECTOR_CAMERA1, draw_target_marker);

    marker.detected = false;
    marker.pixel.x = 0;
    marker.pixel.y = 0;
    marker.found_time = 0;

    detector_locate_helipad();
}

void detector_locate_blob(void)
{
    blob_listener->active = true;
    helipad_listener->active = false;
}

void detector_locate_helipad(void)
{
    blob_listener->active = false;
    helipad_listener->active = true;
}