#include "script/script_events.hpp"

#include "foundation/log.hpp"
#include "pyscript/py_convert.hpp"
#include "pyscript/py_error.hpp"

namespace atlas
{

ScriptEvents::ScriptEvents(PyObjectPtr personality_module) : module_(std::move(personality_module))
{
}

void ScriptEvents::call_module_method(std::string_view name, PyObjectPtr args)
{
    auto method = module_.get_attr(name);
    if (!method || !method.is_callable())
    {
        return;  // method doesn't exist -- silently skip
    }

    PyObjectPtr result;
    if (args)
    {
        result = method.call(args);
    }
    else
    {
        result = method.call();
    }

    if (!result)
    {
        auto err = format_python_error();
        clear_python_error();
        ATLAS_LOG_ERROR("Script callback '{}' failed: {}", name, err);
    }
}

void ScriptEvents::on_init(bool is_reload)
{
    auto args = PyObjectPtr(PyTuple_Pack(1, is_reload ? Py_True : Py_False));
    call_module_method("onInit", args);
}

void ScriptEvents::on_tick(float dt)
{
    auto py_dt = py_convert::to_python(dt);
    // PyTuple_Pack borrows the ref — py_dt must stay alive until after call
    auto args = PyObjectPtr(PyTuple_Pack(1, py_dt.get()));
    call_module_method("onTick", args);
}

void ScriptEvents::on_shutdown()
{
    call_module_method("onShutdown");
}

void ScriptEvents::register_listener(std::string_view event, PyObjectPtr callback)
{
    if (!callback || !callback.is_callable())
    {
        ATLAS_LOG_WARNING("ScriptEvents: ignoring non-callable listener for '{}'", event);
        return;
    }
    listeners_[std::string(event)].push_back(std::move(callback));
}

void ScriptEvents::fire_event(std::string_view event, PyObjectPtr args)
{
    auto it = listeners_.find(std::string(event));
    if (it == listeners_.end())
    {
        return;
    }

    for (auto& cb : it->second)
    {
        PyObjectPtr result;
        if (args)
        {
            result = cb.call(args);
        }
        else
        {
            result = cb.call();
        }
        if (!result)
        {
            auto err = format_python_error();
            clear_python_error();
            ATLAS_LOG_WARNING("Event '{}' listener failed: {}", event, err);
        }
    }
}

}  // namespace atlas
