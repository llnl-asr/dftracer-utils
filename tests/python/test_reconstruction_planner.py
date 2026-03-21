"""Tests for ReconstructionPlannerUtility."""

from dftracer.utils.dftracer_utils_ext import ReconstructionPlannerUtility


class TestReconstructionPlannerUtility:
    def test_plan_empty_input(self):
        result = ReconstructionPlannerUtility().process(reorganized_files=[])
        assert isinstance(result, dict)
        assert "total_segments" in result
        assert "total_events" in result

    def test_call_delegates_to_process(self):
        util = ReconstructionPlannerUtility()
        result = util(reorganized_files=[])
        assert isinstance(result, dict)
        assert "total_segments" in result
