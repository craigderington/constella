import copy
import unittest
from host_temperature import temperature


class HostTemperature(unittest.TestCase):
    now = 1700000000.25
    sample = {"timestamp": "2023-11-14T22:13:20.250000+00:00",
              "temp": {"cpu_temp_avg": 46.0, "gpu_temp_avg": 48.1}}

    def test_hotter_sensor(self):
        self.assertEqual(temperature(self.sample, self.now), 48100)

    def test_expired_and_future_samples(self):
        for offset in (4, -1):
            with self.assertRaises(ValueError):
                temperature(self.sample, self.now + offset)

    def test_invalid_temperatures(self):
        for value in (float("nan"), float("inf"), -1, 0, 150):
            sample = copy.deepcopy(self.sample)
            sample["temp"]["cpu_temp_avg"] = value
            with self.assertRaises(ValueError):
                temperature(sample, self.now)

    def test_missing_sensor(self):
        sample = copy.deepcopy(self.sample)
        del sample["temp"]["cpu_temp_avg"]
        with self.assertRaises(KeyError):
            temperature(sample, self.now)


if __name__ == "__main__":
    unittest.main()
