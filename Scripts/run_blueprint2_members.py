import sys
import unittest

from UnrealMCPython.tests.test_blueprint2_members import TestBlueprint2Members


suite = unittest.defaultTestLoader.loadTestsFromTestCase(TestBlueprint2Members)
result = unittest.TextTestRunner(stream=sys.stdout, verbosity=2).run(suite)
if not result.wasSuccessful():
    raise RuntimeError("Blueprint2 member tests failed")
