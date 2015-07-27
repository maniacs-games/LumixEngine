#include "core/lumix.h"
#include "engine/engine.h"

#include "core/blob.h"
#include "core/crc32.h"
#include "core/input_system.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/resource_manager.h"
#include "core/timer.h"

#include "core/fs/disk_file_device.h"
#include "core/fs/file_system.h"
#include "core/fs/memory_file_device.h"

#include "core/mtjd/manager.h"

#include "debug/debug.h"

#include "engine/plugin_manager.h"

#include "graphics/culling_system.h"
#include "graphics/material_manager.h"
#include "graphics/model_manager.h"
#include "graphics/pipeline.h"
#include "graphics/renderer.h"
#include "graphics/shader_manager.h"
#include "graphics/texture_manager.h"

#include "universe/hierarchy.h"


namespace Lumix
{

static const uint32_t SERIALIZED_ENGINE_MAGIC = 0x5f4c454e; // == '_LEN'


enum class SerializedEngineVersion : int32_t
{
	BASE,

	LATEST // must be the last one
};


#pragma pack(1)
class SerializedEngineHeader
{
public:
	uint32_t m_magic;
	SerializedEngineVersion m_version;
	uint32_t m_reserved; // for crc
};
#pragma pack()

class EngineImpl : public Engine
{
public:
	EngineImpl(const char* base_path, FS::FileSystem* fs, IAllocator& allocator)
		: m_base_path(m_allocator)
		, m_allocator(allocator)
		, m_resource_manager(m_allocator)
		, m_mtjd_manager(m_allocator)
		, m_scenes(m_allocator)
		, m_fps(0)
		, m_editor(nullptr)
	{
		if (!fs)
		{
			m_file_system = FS::FileSystem::create(m_allocator);

			m_mem_file_device =
				m_allocator.newObject<FS::MemoryFileDevice>(m_allocator);
			m_disk_file_device =
				m_allocator.newObject<FS::DiskFileDevice>(m_allocator);

			m_file_system->mount(m_mem_file_device);
			m_file_system->mount(m_disk_file_device);
			m_file_system->setDefaultDevice("memory:disk");
			m_file_system->setSaveGameDevice("memory:disk");
		}
		else
		{
			m_file_system = fs;
			m_mem_file_device = nullptr;
			m_disk_file_device = nullptr;
		}

		m_resource_manager.create(*m_file_system);

		m_timer = Timer::create(m_allocator);
		m_fps_timer = Timer::create(m_allocator);
		m_fps_frame = 0;
		m_universe = nullptr;
		m_hierarchy = nullptr;
		m_base_path = base_path;
	}

	bool create()
	{
		m_plugin_manager = PluginManager::create(*this);
		if (!m_plugin_manager)
		{
			return false;
		}
		m_renderer = Renderer::createInstance(*this);
		if (!m_renderer)
		{
			return false;
		}
		if (!m_renderer->create())
		{
			Renderer::destroyInstance(*m_renderer);
			return false;
		}
		m_plugin_manager->addPlugin(m_renderer);
		if (!m_input_system.create(m_allocator))
		{
			return false;
		}

		return true;
	}


	virtual ~EngineImpl()
	{
		Timer::destroy(m_timer);
		Timer::destroy(m_fps_timer);
		PluginManager::destroy(m_plugin_manager);
		m_input_system.destroy();
		if (m_disk_file_device)
		{
			FS::FileSystem::destroy(m_file_system);
			m_allocator.deleteObject(m_mem_file_device);
			m_allocator.deleteObject(m_disk_file_device);
		}
	}


	virtual IAllocator& getAllocator() override { return m_allocator; }


	virtual Universe* createUniverse() override
	{
		m_universe = m_allocator.newObject<Universe>(m_allocator);
		m_hierarchy = Hierarchy::create(*m_universe, m_allocator);
		const Array<IPlugin*>& plugins = m_plugin_manager->getPlugins();
		for (auto* plugin : plugins)
		{
			IScene* scene = plugin->createScene(*m_universe);
			if (scene)
			{
				m_scenes.push(scene);
			}
		}

		return m_universe;
	}


	virtual IScene* getSceneByComponentType(uint32_t type) const override
	{
		for (int i = 0; i < m_scenes.size(); ++i)
		{
			if (m_scenes[i]->ownComponentType(type))
			{
				return m_scenes[i];
			}
		}
		return nullptr;
	}


	virtual IScene* getScene(uint32_t type) const override
	{
		for (int i = 0; i < m_scenes.size(); ++i)
		{
			if (crc32(m_scenes[i]->getPlugin().getName()) == type)
			{
				return m_scenes[i];
			}
		}
		return nullptr;
	}


	virtual MTJD::Manager& getMTJDManager() override { return m_mtjd_manager; }


	virtual const Array<IScene*>& getScenes() const override
	{
		return m_scenes;
	}


	virtual void destroyUniverse() override
	{
		ASSERT(m_universe);
		if (m_universe)
		{
			for (int i = m_scenes.size() - 1; i >= 0; --i)
			{
				m_scenes[i]->getPlugin().destroyScene(m_scenes[i]);
			}
			m_scenes.clear();
			Hierarchy::destroy(m_hierarchy);
			m_hierarchy = nullptr;
			m_allocator.deleteObject(m_universe);
			m_universe = nullptr;
		}
	}

	virtual void setWorldEditor(WorldEditor& editor) override
	{
		m_editor = &editor;
		for (auto* plugin : m_plugin_manager->getPlugins())
		{
			plugin->setWorldEditor(editor);
		}
	}


	virtual WorldEditor* getWorldEditor() const override { return m_editor; }


	virtual PluginManager& getPluginManager() override
	{
		return *m_plugin_manager;
	}


	virtual FS::FileSystem& getFileSystem() override { return *m_file_system; }


	virtual Renderer& getRenderer() override { return *m_renderer; }


	void updateGame(float dt)
	{
		PROFILE_FUNCTION();
		for (int i = 0; i < m_scenes.size(); ++i)
		{
			m_scenes[i]->update(dt);
		}
		m_plugin_manager->update(dt);
		m_input_system.update(dt);
	}


	virtual void update(bool is_game_running,
						float time_delta_multiplier,
						float forced_time_delta) override
	{
		PROFILE_FUNCTION();
		float dt;
		if (forced_time_delta >= 0)
		{
			dt = forced_time_delta * time_delta_multiplier;
			m_fps_frame = 0;
			m_fps = forced_time_delta * time_delta_multiplier == 0
						? 0
						: 1.0f / (forced_time_delta * time_delta_multiplier);
			m_fps_timer->tick();
		}
		else
		{
			++m_fps_frame;
			if (m_fps_frame == 30)
			{
				m_fps = 30.0f / m_fps_timer->tick();
				m_fps_frame = 0;
			}
			dt = m_timer->tick() * time_delta_multiplier;
		}
		m_last_time_delta = dt;
		if (is_game_running)
		{

			updateGame(dt);
		}
		else
		{
			for (int i = 0; i < m_scenes.size(); ++i)
			{
				if (&m_scenes[i]->getPlugin() == m_renderer)
				{
					m_scenes[i]->update(dt);
				}
			}
		}
		getFileSystem().updateAsyncTransactions();
	}


	virtual IPlugin* loadPlugin(const char* name) override
	{
		return m_plugin_manager->load(name);
	}


	virtual InputSystem& getInputSystem() override { return m_input_system; }


	virtual const char* getBasePath() const override
	{
		return m_base_path.c_str();
	}


	virtual Universe* getUniverse() const override { return m_universe; }


	virtual Hierarchy* getHierarchy() const override { return m_hierarchy; }


	virtual ResourceManager& getResourceManager() override
	{
		return m_resource_manager;
	}


	virtual float getFPS() const override { return m_fps; }


	virtual uint32_t serialize(OutputBlob& serializer) override
	{
		SerializedEngineHeader header;
		header.m_magic = SERIALIZED_ENGINE_MAGIC; // == '_LEN'
		header.m_version = SerializedEngineVersion::LATEST;
		header.m_reserved = 0;
		serializer.write(header);
		g_path_manager.serialize(serializer);
		int pos = serializer.getSize();
		m_universe->serialize(serializer);
		m_hierarchy->serialize(serializer);
		m_renderer->serialize(serializer);
		m_plugin_manager->serialize(serializer);
		for (int i = 0; i < m_scenes.size(); ++i)
		{
			m_scenes[i]->serialize(serializer);
		}
		uint32_t crc = crc32((const uint8_t*)serializer.getData() + pos,
							 serializer.getSize() - pos);
		return crc;
	}


	virtual bool deserialize(InputBlob& serializer) override
	{
		SerializedEngineHeader header;
		serializer.read(header);
		if (header.m_magic != SERIALIZED_ENGINE_MAGIC)
		{
			g_log_error.log("engine") << "Wrong or corrupted file";
			return false;
		}
		if (header.m_version > SerializedEngineVersion::LATEST)
		{
			g_log_error.log("engine") << "Unsupported version";
			return false;
		}
		g_path_manager.deserialize(serializer);
		m_universe->deserialize(serializer);
		m_hierarchy->deserialize(serializer);
		m_renderer->deserialize(serializer);
		m_plugin_manager->deserialize(serializer);
		for (int i = 0; i < m_scenes.size(); ++i)
		{
			m_scenes[i]->deserialize(serializer);
		}
		return true;
	}


	virtual float getLastTimeDelta() override { return m_last_time_delta; }


private:
	IAllocator& m_allocator;

	Renderer* m_renderer;
	FS::FileSystem* m_file_system;
	FS::MemoryFileDevice* m_mem_file_device;
	FS::DiskFileDevice* m_disk_file_device;

	ResourceManager m_resource_manager;

	MTJD::Manager m_mtjd_manager;

	string m_base_path;
	WorldEditor* m_editor;
	PluginManager* m_plugin_manager;
	Universe* m_universe;
	Hierarchy* m_hierarchy;
	Array<IScene*> m_scenes;
	InputSystem m_input_system;
	Timer* m_timer;
	Timer* m_fps_timer;
	int m_fps_frame;
	float m_fps;
	float m_last_time_delta;

private:
	void operator=(const EngineImpl&);
	EngineImpl(const EngineImpl&);
};


void showLogInVS(const char*, const char* message)
{
	Debug::debugOutput(message);
	Debug::debugOutput("\n");
}


Engine*
Engine::create(const char* base_path, FS::FileSystem* fs, IAllocator& allocator)
{
	installUnhandledExceptionHandler(base_path);

	g_log_info.getCallback().bind<showLogInVS>();
	g_log_warning.getCallback().bind<showLogInVS>();
	g_log_error.getCallback().bind<showLogInVS>();

	EngineImpl* engine =
		allocator.newObject<EngineImpl>(base_path, fs, allocator);
	if (!engine->create())
	{
		allocator.deleteObject(engine);
		return nullptr;
	}
	return engine;
}


void Engine::destroy(Engine* engine)
{
	engine->getAllocator().deleteObject(engine);
}


} // ~namespace Lumix