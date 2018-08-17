#!/usr/bin/env python3

import sys
import os
import os.path
import numpy as np
import scipy.optimize
import math
from fractions import Fraction as F

def to_euclidean(gps_coords):
    # Add to python path the gps_converter module,
    # built with script georeferencer_build.py
    sys.path.append('./build')

    import gps_converter as gc
    ptr = gc.ffi.cast("long double *", gps_coords.ctypes.data)
    gc.lib.gps_to_euclidean(len(gps_coords), ptr, ptr)

def to_frac(n):
    """Converts exifread rational to python's fraction"""
    return F(n.num, n.den)

def to_degrees(vals):
    return to_frac(vals[0]) + to_frac(vals[1]) / 60 + to_frac(vals[2]) / 3600

def maybe_flip(val, ref, positive, negative):
    assert(ref in [positive, negative])

    if ref == negative:
        return -val
    else:
        return val

def load_camera_coords(scene_dir):
    import configparser
    import exifread

    views_dir = os.path.join(scene_dir, 'views')
    views = os.listdir(views_dir)

    model_coords = np.empty([len(views), 3], dtype=np.longdouble)
    gps_coords = np.empty([len(views), 3], dtype=np.longdouble)

    for i, d in enumerate(views):
        # Load model coords
        metaname = os.path.join(views_dir, d, 'meta.ini')
        meta = configparser.ConfigParser()
        meta.read(metaname)

        # Load camera rotation:
        rot = np.fromstring(
            meta['camera']['rotation'],
            dtype=np.longdouble, sep=' '
        )
        # Use Fortran indexing to transpose the matrix.
        # Since this is a rotation matrix, the transpose
        # equals the inverse.
        rot = np.reshape(rot, [3, 3], 'F')

        # Calculate camera center as c = -R^(-1) * t, as explained in:
        # https://github.com/simonfuhrmann/mve/wiki/Math-Cookbook
        model_coords[i] = -np.matmul(rot, np.fromstring(
            meta['camera']['translation'],
            dtype=np.longdouble, sep=' '
        ))

        # Load GPS coords
        imgpath = os.path.join(views_dir, d, 'original.jpg')
        img = open(imgpath, 'rb')

        exif = exifread.process_file(img, details=False)

        lat = to_degrees(exif['GPS GPSLatitude'].values)
        lat = maybe_flip(lat, exif['GPS GPSLatitudeRef'].values, 'N', 'S')

        lon = to_degrees(exif['GPS GPSLongitude'].values)
        lon = maybe_flip(lon, exif['GPS GPSLongitudeRef'].values, 'E', 'W')

        alt = to_frac(exif['GPS GPSAltitude'].values[0])
        alt = maybe_flip(alt, exif['GPS GPSAltitudeRef'].values[0], 0, 1)

        # Convert from rational to long double (I hope)
        gps_coords[i] = (
            np.array([lat.numerator,   lon.numerator,   alt.numerator  ], dtype=np.longdouble) /
            np.array([lat.denominator, lon.denominator, alt.denominator], dtype=np.longdouble)
        )

    return (gps_coords, model_coords)

def x_rot(angle):
    c = math.cos(angle)
    s = math.sin(angle)
    return np.array([
        [1.0, 0.0, 0.0],
        [0.0,   c,  -s],
        [0.0,   s,   c]
    ])

def y_rot(angle):
    c = math.cos(angle)
    s = math.sin(angle)
    return np.array([
        [  c, 0.0,   s],
        [0.0, 1.0, 0.0],
        [ -s, 0.0,   c]
    ])

def z_rot(angle):
    c = math.cos(angle)
    s = math.sin(angle)
    return np.array([
        [  c,  -s, 0.0],
        [  s,   c, 0.0],
        [0.0, 0.0, 1.0]
    ])

def transform(t, point):
    return  np.matmul(z_rot(t[6]),
            np.matmul(y_rot(t[5]),
            np.matmul(x_rot(t[4]),
                point*t[0] + t[1:4]
            )))

def model_to_gps_sq_error(transformation, gps_coords, model_coords):
    err = 0.0
    for m, g in zip(model_coords, gps_coords):
        v = transform(transformation, m) - g
        # TODO: check which leads to smaller error...
        err += np.dot(v, v) #np.sum(np.abs(v))
    return err

def main():
    if len(sys.argv) < 1:
        print('Error: Missing arguments. Usage:\n'
                + sys.argv[0] + ' <mve-scene-dir>')
        sys.exit(1)

    # Load the two set of coordinates:
    scene_dir = sys.argv[1]

    try:
        gps_coords, model_coords = load_camera_coords(scene_dir)
    except FileNotFoundError as e:
        print('Error loading coordinates:')
        print(' -', e)
        sys.exit(1)

    # Convert the gps coordinates to euclidean space
    to_euclidean(gps_coords)

    # Since the translation is not important, we subtract
    # one of the points so the values are around point (0,0,0).
    for e in gps_coords[1:]:
        e -= gps_coords[0]
    gps_coords[0] = np.zeros_like(gps_coords[0])

    # Now we minimize the error of the transformation from
    # model space to gps space, to find the scale and the
    # rotation.

    # Setup initial guess from what is easy to calculate:
    scale = (np.linalg.norm(-gps_coords[1])
            / np.linalg.norm(model_coords[0] - model_coords[1]))

    translation = -scale * model_coords[0]

    transformation = np.zeros([7])
    transformation[0] = scale
    transformation[1:4] = translation

    # Run the optimization:
    result = scipy.optimize.minimize(model_to_gps_sq_error,
        transformation, (gps_coords, model_coords))
    print(result)

    print('Error: ',
        model_to_gps_sq_error(result.x, gps_coords, model_coords)**0.5,
        'm'
    )

    # Take the results and print:
    scale = result.x[0]
    translation = result.x[1:4]
    euler_angles = result.x[4:]

    print('Scale: ', scale)
    print('Translation: ', translation)
    print('Euler angle rotation: ', euler_angles)

if __name__ == "__main__":
    main()
