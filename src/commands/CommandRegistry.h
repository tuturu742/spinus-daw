#pragma once

#include "Command.h"

#include <functional>

namespace juce
{
class AudioProcessor;
}

namespace spinus::commands
{

/** How device.setParameters reads 'text' for a device that is not Spinus's own (a bundled plugin, any VST3,
    CLAP, LV2 or VST): the plugin as it plays in the project, or a copy made for reading (a device added in the
    same proposal is not playing yet). Nothing when it cannot be loaded here. */
using PluginForText = std::function<std::shared_ptr<juce::AudioProcessor> (const model::Device&)>;

/** The Session that runs commands gives its plugins (message thread). The last one set is used; an owner
    clears only its own. Without one (a command history on its own), such text is refused: values only. */
void setPluginForText (const void* owner, PluginForText);
void clearPluginForText (const void* owner);

/** Builds a JSON Schema (draft 2020-12) object for a command's parameters. */
class ParameterSchema
{
public:
    ParameterSchema& id (const char* name, const char* description, bool required = true);
    ParameterSchema& number (const char* name, const char* description, bool required, double min, double max);
    ParameterSchema& integer (const char* name, const char* description, bool required, int min, int max);
    ParameterSchema& string (const char* name, const char* description, bool required = false);
    ParameterSchema& boolean (const char* name, const char* description);
    ParameterSchema& custom (const char* name, const char* description, bool required, const juce::var& schema);

    juce::var build() const;

private:
    juce::DynamicObject::Ptr properties = new juce::DynamicObject();
    juce::StringArray required;
};

struct CommandInfo
{
    juce::String name;
    juce::String summary; // one sentence, shown to the LLM and in the docs
    juce::var parameterSchema;

    /** Creates the command from JSON parameters, or returns nullptr and sets `error`. */
    std::function<std::unique_ptr<Command> (const juce::var& parameters, juce::String& error)> create;
};

/** Every command Spinus knows. The same set serves the UI, the AI service (JSON-RPC
    `project.v1.apply`) and scripted tests. docs/dev/commands.schema.json is generated from it, and a
    test fails if the checked-in copy is out of date. */
class CommandRegistry
{
public:
    static const CommandRegistry& get();

    const std::vector<CommandInfo>& getCommands() const noexcept { return commands; }
    const CommandInfo* find (const juce::String& name) const;

    /** Creates a command, or nullptr with `error` set (unknown name or bad parameters). */
    std::unique_ptr<Command> create (const juce::String& name, const juce::var& parameters, juce::String& error) const;

    /** Creates a Transaction from {"description": "...", "commands": [{"command": name, "parameters": {...}}]}. */
    std::unique_ptr<Command> createTransaction (const juce::var& json, juce::String& error) const;

    /** The JSON Schema of every command's parameters. */
    juce::var getSchema() const;

private:
    CommandRegistry();
    std::vector<CommandInfo> commands;
};

} // namespace spinus::commands
