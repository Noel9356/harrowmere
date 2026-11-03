#include "engine/kernel/component_interface.hpp"
#include "engine/kernel/component_manifest.hpp"
#include "engine/kernel/mod_api.hpp"

namespace
{

class ExampleSpeciesMod final : public engine::ModInterface
{
public:
    ExampleSpeciesMod()
    {
        manifest_.name    = "example_species_mod";
        manifest_.kind    = engine::ComponentKind::SharedLibraryMod;
        manifest_.version = "0.1.0";
    }

    const engine::ComponentManifest &manifest() const override
    {
        return manifest_;
    }
    void construct(
        engine::ComponentContext &
    ) override
    {}
    void start() override
    {}
    void stop() override
    {}
    void *capabilityPointerFor(
        const std::string &
    ) override
    {
        return nullptr;
    }

private:
    engine::ComponentManifest manifest_{};
};

engine::ModInterface *createMod()
{
    return new ExampleSpeciesMod();
}
void destroyMod(
    engine::ModInterface *mod
)
{
    delete mod;
}

const EngineModEntryTable entryTable{
    .interfaceVersion = engine::engineModInterfaceVersion,
    .createMod        = &createMod,
    .destroyMod       = &destroyMod,
};

} // namespace

extern "C" const EngineModEntryTable *engine_query_mod_entry_table()
{
    return &entryTable;
}
