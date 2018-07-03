import ephem
import datetime
import math

def year_angles(latitude, longitude, elevation):
    """
    Yields the unit vector pointing to the sun over the year of 2017, for
    every 5 minutes of daylight.

    Result is a tuple.
    """

    obs = ephem.Observer()
    obs.lat = str(latitude)
    obs.lon = str(longitude)
    obs.elevation = elevation

    dt = datetime.timedelta(minutes=5)

    t = datetime.datetime(2017, 1, 1, 0, 0, 0)

    final = datetime.datetime(2018, 1, 1, 0, 0, 0)

    while(t <= final):
        obs.date = t.strftime('%Y/%m/%d %H:%M:%S')
        v = ephem.Sun(obs)

        if v.alt > 0.0:
            y = math.sin(v.alt)
            compl = math.cos(v.alt)
            x = math.sin(v.az) * compl
            z = math.cos(v.az) * compl

            yield (x, y, z)

        t += dt
