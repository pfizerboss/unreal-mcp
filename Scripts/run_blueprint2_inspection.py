import sys
import unittest

from UnrealMCPython.tests.test_blueprint2_inspection import (
    TestBlueprint2Inspection,
)


suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestBlueprint2Inspection)
result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
if not result.wasSuccessful():
    raise RuntimeError("Blueprint2 inspection tests failed")
