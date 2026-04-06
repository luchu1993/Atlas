#include <gtest/gtest.h>
#include "pyscript/atlas_module.hpp"
#include "pyscript/py_interpreter.hpp"
#include "foundation/log.hpp"

using namespace atlas;

class AtlasModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
            (void)PyInterpreter::initialize();

        static bool module_registered = false;
        if (!module_registered)
        {
            auto result = register_atlas_module();
            ASSERT_TRUE(result.has_value()) << result.error().message();
            module_registered = true;
        }
    }
};

TEST_F(AtlasModuleTest, ImportAtlasModule)
{
    auto mod = PyInterpreter::import("atlas");
    ASSERT_TRUE(mod.has_value()) << mod.error().message();
}

TEST_F(AtlasModuleTest, LogInfoFromPython)
{
    auto result = PyInterpreter::exec("import atlas\natlas.log_info('Hello from Python!')");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(AtlasModuleTest, LogWarningFromPython)
{
    auto result = PyInterpreter::exec("import atlas\natlas.log_warning('Warning from script')");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(AtlasModuleTest, ServerTimeReturnsFloat)
{
    auto result = PyInterpreter::exec(
        "import atlas\n"
        "t = atlas.server_time()\n"
        "assert isinstance(t, float), f'Expected float, got {type(t)}'\n"
        "assert t > 0, f'Expected positive time, got {t}'\n"
    );
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(AtlasModuleTest, VersionConstants)
{
    auto result = PyInterpreter::exec(
        "import atlas\n"
        "assert atlas.VERSION_MAJOR == 0\n"
        "assert atlas.VERSION_MINOR == 1\n"
        "assert atlas.ENGINE_NAME == 'Atlas'\n"
    );
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(AtlasModuleTest, LogErrorFromPython)
{
    auto result = PyInterpreter::exec("import atlas\natlas.log_error('Error from script')");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}
