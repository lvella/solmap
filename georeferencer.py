#!/usr/bin/env python3

import sys
import os
import os.path
import numpy as np
import scipy.optimize
import math
from pyquaternion import Quaternion
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

    model_coords = []
    gps_coords = []

    for i, d in enumerate(views):
        # Load model coords
        metaname = os.path.join(views_dir, d, 'meta.ini')
        meta = configparser.ConfigParser()
        meta.read(metaname)

        # Load camera rotation:
        try:
            rot = np.fromstring(
                meta['camera']['rotation'],
                dtype=np.longdouble, sep=' '
            )
        except KeyError as k:
            print("Warning: file skipped:", metaname)
            continue

        # Use Fortran indexing to transpose the matrix.
        # Since this is a rotation matrix, the transpose
        # equals the inverse.
        rot = np.reshape(rot, [3, 3], 'F')

        # Calculate camera center as c = -R^(-1) * t, as explained in:
        # https://github.com/simonfuhrmann/mve/wiki/Math-Cookbook
        model_coords.append(-np.matmul(rot, np.fromstring(
            meta['camera']['translation'],
            dtype=np.longdouble, sep=' '
        )))

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
        gps_coords.append(
            np.array([lat.numerator,   lon.numerator,   alt.numerator  ], dtype=np.longdouble) /
            np.array([lat.denominator, lon.denominator, alt.denominator], dtype=np.longdouble)
        )

    return (np.array(gps_coords, dtype=np.longdouble),
            np.array(model_coords, dtype=np.longdouble))

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

def rotate(euler_angles, v):
    return  np.matmul(z_rot(euler_angles[2]),
            np.matmul(y_rot(euler_angles[1]),
            np.matmul(x_rot(euler_angles[0]),
                v
            )))

def transform(t, point):
    return  t[1:4] + t[0] * rotate(t[4:], point)

def model_to_gps_sq_error(transformation, gps_coords, model_coords):
    err = 0.0
    for g, m in zip(gps_coords, model_coords):
        v = transform(transformation, m) - g
        # Don't know why, but least squares seems
        # to give a better orientation than least
        # absolute difference.
        err += np.dot(v, v)
        #err += math.sqrt(np.dot(v, v))
    return err

def find_median_coords(gps_coords):
    hi = gps_coords[0,:2]
    lo = hi

    for p in gps_coords[1:]:
        lo = np.minimum(lo, p[:2])
        hi = np.maximum(hi, p[:2])

    return (lo + hi) * 0.5

def main():
    if len(sys.argv) < 3:
        print('Error: Missing arguments. Usage:\n'
                + sys.argv[0] + ' <mve-scene-dir> <3d-mesh>')
        sys.exit(1)

    scene_dir = sys.argv[1]
    mesh_file = sys.argv[2]

    # Load the two set of coordinates:
    try:
        gps_coords, model_coords = load_camera_coords(scene_dir)
    except FileNotFoundError as e:
        print('Error loading coordinates:')
        print(' -', e)
        sys.exit(1)

    # Find the median latitude and longitude:
    median = find_median_coords(gps_coords)
    print('Median coordinates:', median)

    # Convert the gps coordinates to euclidean space;
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
    del transformation

    print('Transformation error:',
        model_to_gps_sq_error(result.x, gps_coords, model_coords)**0.5,
        'm'
    )

    # Take the results:
    scale = result.x[0]
    translation = result.x[1:4]
    from_model_to_gps = rotate(result.x[4:],
            np.identity(3, dtype=np.longdouble))

    # Rotation matrix from libwgs84 coordinates to colmap
    # coordinates, where up is +y and north is -z.
    libwgs84_to_colmap = np.array([
        [ 0,  1,  0],
        [ 1,  0,  0],
        [ 0,  0, -1]],
        dtype=np.longdouble
    )

    # Rotation from gps coordinates back to latitude 0 and longitude 0:
    to_rad = math.pi / 180.0
    rotate_to_origin = np.matmul(
        y_rot(median[0] * to_rad),
        z_rot(-median[1] * to_rad)
    )

    # Adjust the result rotation to colmap coordinates:
    rotation = np.matmul(libwgs84_to_colmap,
               np.matmul(rotate_to_origin,
                         from_model_to_gps
    ))

    # Print the result:
    quat = Quaternion(matrix=rotation.astype(np.double))
    print('Scale:', scale)
    print('Translation:', translation)
    print('Rotation quaternion:', quat)

    script_path = os.path.dirname(os.path.realpath(__file__))

    executable = os.path.join(script_path, 'build', 'solmap')
    os.execl(executable, executable,
        '--rotation-quaternion={}:{}:{}:{}'.format(*quat),
        '--scale={}'.format(scale),
        '{}'.format(median[0]), '{}'.format(median[1]),
        mesh_file)

if __name__ == "__main__":
    main()
