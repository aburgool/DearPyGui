#include "mvApp.h"
#include "mvMarvel.h"
#include "mvCore.h"
#include "mvDataStorage.h"
#include "mvInput.h"
#include "mvTextEditor.h"
#include "mvThemeScheme.h"
#include <fstream>
#include <streambuf>
#include "mvAppLog.h"
#include "Core/StandardWindows/mvDocWindow.h"
#include "Core/StandardWindows/mvAboutWindow.h"
#include "Core/StandardWindows/mvMetricsWindow.h"
#include "Core/StandardWindows/mvSourceWindow.h"
#include "Core/StandardWindows/mvDebugWindow.h"
#include "Core/StandardWindows/mvFileDialog.h"
#include "Core/StandardWindows/mvStyleWindow.h"
#include <thread>
#include <future>
#include <chrono>
#include "Core/mvThreadPool.h"
#include "Core/AppItems/mvAppItems.h"
#include <frameobject.h>

namespace Marvel {

	mvApp* mvApp::s_instance = nullptr;
	bool   mvApp::s_started = false;

	mvApp* mvApp::GetApp()
	{
		if (s_instance)
			return s_instance;

		s_instance = new mvApp();
		return s_instance;
	}

	void mvApp::DeleteApp()
	{
		if (s_instance)
		{
			delete s_instance;
			s_instance = nullptr;
		}

		s_started = false;
	}

	mvStandardWindow* mvApp::GetAppStandardWindow()
	{
		return static_cast<mvStandardWindow*>(GetApp());
	}

	mvApp::mvApp()
	{
		m_parsers = BuildDearPyGuiInterface();

		// info
		mvAppLog::AddLog("\n");
		mvAppLog::AddLog("[DearPyGui Version] %0s\n", mvApp::GetVersion());
		mvAppLog::AddLog("[Python Version] %0s\n", PY_VERSION);
		mvAppLog::AddLog("[DearImGui Version] %0s\n", IMGUI_VERSION);
#if defined (_WIN32)
		    mvAppLog::AddLog("[Compiler] MSVC version %0d\n", _MSC_VER);
#endif

		m_mainThreadID = std::this_thread::get_id();

		m_windows.push_back(new mvWindowAppitem("", "MainWindow", 1280, 800, 0, 0, true, false, true, false, false));
		m_parents.push(m_windows.back());

		addStandardWindow("documentation", mvDocWindow::GetWindow());
		addStandardWindow("about", new mvAboutWindow());
		addStandardWindow("metrics", new mvMetricsWindow());
		addStandardWindow("source", new mvSourceWindow());
		addStandardWindow("debug", new mvDebugWindow());
		addStandardWindow("filedialog", new mvFileDialog());
		addStandardWindow("style", new mvStyleWindow());

	}

	mvApp::~mvApp()
	{
		for (auto window : m_windows)
		{
			delete window;
			window = nullptr;
		}
		m_windows.clear();

		mvTextureStorage::DeleteAllTextures();
		mvDataStorage::DeleteAllData();
	}

	void mvApp::precheck()
	{
		// prevents the user from having to call
		// end_window when there is only the main one
		if (m_windows.size() == 1)
			popParent();

		// If any data was stored during compile time,
		// this will update items relying on it before
		// the first render frame
		mvDataStorage::UpdateData();

	}

	void mvApp::firstRenderFrame()
	{
		m_firstRender = false;

		// if any theme color is not specified, use the default colors
		for (int i = 0; i < ImGuiCol_COUNT; i++)
			if (m_newstyle.Colors[i].x == 0.0f && m_newstyle.Colors[i].y == 0.0f &&
				m_newstyle.Colors[i].z == 0.0f && m_newstyle.Colors[i].w == 0.0f)
				m_newstyle.Colors[i] = ImGui::GetStyle().Colors[i];

		// if theme was set during compile time, actually make the change now
		if (m_compileTimeThemeSet)
		{
			changeTheme();
			m_compileTimeThemeSet = false;
		}
	}

	void mvApp::prerender()
	{

		if (m_firstRender)
			firstRenderFrame();

		// check if threadpool is ready to be cleaned up
		if (m_threadTime > m_threadPoolTimeout)
		{
			if (m_tpool != nullptr)
			{
				delete m_tpool;
				m_tpool = nullptr;
				m_threadTime = 0.0;
				m_threadPool = false;
				mvAppLog::Log("Threadpool destroyed");
			}

		}

		// update timing
		m_deltaTime = ImGui::GetIO().DeltaTime;
		m_time = ImGui::GetTime();
		ImGui::GetIO().FontGlobalScale = m_globalFontScale;

		// check if any asyncronous functions have returned
		// and are requesting to send data back to main thread
		if (!m_asyncReturns.empty())
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			while (!m_asyncReturns.empty())
			{
				auto& asyncreturn = m_asyncReturns.front();
				runReturnCallback(asyncreturn.name, "Asyncrounous Callback", asyncreturn.data);
				m_asyncReturns.pop();
			}
		}

		mvAppLog::render();

		// set imgui style to mvstyle
		if (m_styleChange)
			updateStyle();

		// route any registered input callbacks
		routeInputCallbacks();

		// run render callbacks
		if (m_activeWindow == "MainWindow")
		{
			if (!getRenderCallback().empty())
				runCallback(getRenderCallback(), "Main Application");
		}

		else
		{
			mvAppItem* item = mvApp::GetApp()->getItem(m_activeWindow);

			if (item == nullptr)
				m_activeWindow = "MainWindow";
			else
			{
				dispatchRenderCallback<mvWindowAppitem>(mvAppItemType::Window, item);
				dispatchRenderCallback<mvChild>(mvAppItemType::Child, item);
				dispatchRenderCallback<mvPopup>(mvAppItemType::Popup, item);
				dispatchRenderCallback<mvMenu>(mvAppItemType::Menu, item);
			}
		}

		// resets app items states (i.e. hovered)
		for (auto window : m_windows)
			window->resetState();

	}

	void mvApp::render(bool& show)
	{
		for (auto window : m_windows)
			window->draw();
	}

	void mvApp::postrender()
	{

		// render any standard windows (i.e. debug, etc.)
		for (auto& entry : m_standardWindows)
		{
			if (entry.second.show)
				entry.second.window->render(entry.second.show);
		}

		Py_BEGIN_ALLOW_THREADS

		// delete items from the delete queue
		while (!m_deleteChildrenQueue.empty())
		{
			auto item = getItem(m_deleteChildrenQueue.front());
			if (item)
				item->deleteChildren();
			m_deleteChildrenQueue.pop();
		}

		// delete items from the delete queue
		while (!m_deleteQueue.empty())
		{
			bool deletedItem = false;

			// try to delete item
			for (auto window : m_windows)
			{
				deletedItem = window->deleteChild(m_deleteQueue.front());
				if (deletedItem)
					break;
			}

			bool windowDeleting = false;

			// check if attempting to delete a window
			for (auto window : m_windows)
			{
				if (window->getName() == m_deleteQueue.front())
				{
					windowDeleting = true;
					break;
				}
			}

			// delete window and update window vector
			// this should be changed to a different data
			// structure
			if (windowDeleting)
			{
				std::vector<mvAppItem*> oldwindows = m_windows;

				m_windows.clear();

				for (auto window : oldwindows)
				{
					if (window->getName() == m_deleteQueue.front())
					{
						delete window;
						window = nullptr;
						deletedItem = true;
						continue;
					}
					m_windows.push_back(window);
				}
			}

			if (!deletedItem)
				ThrowPythonException(m_deleteQueue.front() + " not deleted because it was not found");

			m_deleteQueue.pop();
		}

		// add runtime items
		for (auto& newItem : m_newItemVec)
		{

			bool addedItem = false;

			if (getItem(newItem.item->getName(), true))
			{
				std::string message = newItem.item->getName();
				ThrowPythonException(message + ": Items of this type must have unique names");
				delete newItem.item;
				newItem.item = nullptr;
				continue;
			}

			if (newItem.item->getType() == mvAppItemType::Window)
			{
				m_windows.push_back(newItem.item);
				continue;
			}

			for (auto window : m_windows)
			{
				addedItem = window->addRuntimeChild(newItem.parent, newItem.before, newItem.item);
				if (addedItem)
					break;
			}

			if (!addedItem)
			{
				ThrowPythonException(newItem.item->getName() + " not added because its parent was not found");
				delete newItem.item;
				newItem.item = nullptr;
			}

		}

		m_newItemVec.clear();

		// move items up
		while (!m_upQueue.empty())
		{
			std::string& itemname = m_upQueue.front();

			bool movedItem = false;

			for (auto window : m_windows)
			{
				movedItem = window->moveChildUp(itemname);
				if (movedItem)
					break;
			}

			if (!movedItem)
				ThrowPythonException(itemname + " not moved because it was not found");

			m_upQueue.pop();
		}

		// move items down
		while (!m_downQueue.empty())
		{
			std::string& itemname = m_downQueue.front();

			bool movedItem = false;

			for (auto window : m_windows)
			{
				movedItem = window->moveChildDown(itemname);
				if (movedItem)
					break;
			}

			if (!movedItem)
				ThrowPythonException(itemname + " not moved because it was not found");

			m_downQueue.pop();
		}

		// async callbacks
		if (!m_asyncCallbacks.empty())
		{
			// check if threadpool is valid, if not, create it
			if (m_tpool == nullptr)
			{
				m_tpool = new mvThreadPool(m_threadPoolHighPerformance ? 0 : m_threads);
				m_poolStart = clock_::now();
				m_threadPool = true;
				mvAppLog::Log("Threadpool created");
			}


			// submit to thread pool
			for (auto& callback : m_asyncCallbacks)
				m_tpool->submit(std::bind(&mvApp::runAsyncCallback, this, callback.name, callback.data, callback.returnname));

			//std::lock_guard<std::mutex> lock(m_mutex);
			m_asyncCallbacks.clear();
		}

		// update timer if thread pool exists
		if (m_tpool != nullptr)
			m_threadTime = std::chrono::duration_cast<second_>(clock_::now() - m_poolStart).count();

		Py_END_ALLOW_THREADS
	}

	void mvApp::setWindowSize(unsigned width, unsigned height)
	{

		// set viewport size
		setSize(width, height);

		// set imgui window size
		m_windows[0]->setWidth(width);
		m_windows[0]->setHeight(height);
	}

	void mvApp::setActualSize(unsigned width, unsigned height)
	{

		m_actualWidth = width;
		m_actualHeight = height;
	}

	void mvApp::setGlobalFontScale(float scale)
	{
		m_globalFontScale = scale;
	}

	void mvApp::setFont(const std::string& file, float size, const std::string& glyphRange)
	{
		m_fontFile = file;
		m_fontGlyphRange = glyphRange;
		m_fontSize = size;
	}

	void mvApp::updateStyle()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		style = m_newstyle;
		m_styleChange = false;
	}

	bool mvApp::checkIfMainThread() const
	{
		if (std::this_thread::get_id() != m_mainThreadID)
		{
			int line = PyFrame_GetLineNumber(PyEval_GetFrame());
			PyErr_Format(PyExc_Exception,
				"DearPyGui command on line %d can not be called asycronously", line);
			PyErr_Print();
			return false;
		}
		return true;
	}

	void mvApp::routeInputCallbacks()
	{
		// Note: Events are only routed to the active window

		// default handler is main window
		mvEventHandler* eventHandler = static_cast<mvEventHandler*>(this);
		if (m_activeWindow != "MainWindow")
		{
			for (auto window : m_windows)
			{
				if (window->getName() == m_activeWindow)
				{
					auto windowtype = static_cast<mvWindowAppitem*>(window);
					eventHandler = static_cast<mvEventHandler*>(windowtype);
					break;
				}
			}
		}

		// early opt out of keyboard events
		if (eventHandler->isKeyboardHandled())
		{
			// route key events
			for (int i = 0; i < IM_ARRAYSIZE(ImGui::GetIO().KeysDown); i++)
			{
				// route key pressed event
				if (ImGui::IsKeyPressed(i) && !eventHandler->getKeyPressCallback().empty())
					runCallback(eventHandler->getKeyPressCallback(), std::to_string(i));

				// route key down event
				if (ImGui::GetIO().KeysDownDuration[i] >= 0.0f && !eventHandler->getKeyDownCallback().empty())
					runCallback(eventHandler->getKeyDownCallback(), std::to_string(i),
						mvPythonTranslator::ToPyFloat(ImGui::GetIO().KeysDownDuration[i]));

				// route key released event
				if (ImGui::IsKeyReleased(i) && !eventHandler->getKeyReleaseCallback().empty())
					runCallback(eventHandler->getKeyReleaseCallback(), std::to_string(i));
			}
		}

		// early opt out of mouse events
		if (!eventHandler->isMouseHandled())
			return;

		// route mouse wheel event
		if (ImGui::GetIO().MouseWheel != 0.0f && !eventHandler->getMouseWheelCallback().empty())
			runCallback(eventHandler->getMouseWheelCallback(), m_activeWindow,
				mvPythonTranslator::ToPyMPair(0, ImGui::GetIO().MouseWheel));

		// route mouse dragging event
		// this must be seperate since only a single button can be dragged
		if (!eventHandler->getMouseDragCallback().empty())
		{
			for (int i = 0; i < 3; i++)
			{
				if (ImGui::IsMouseDragging(i, mvInput::getMouseDragThreshold()))
				{
					// TODO: send delta
					mvInput::setMouseDragging(true);
					mvInput::setMouseDragDelta({ ImGui::GetMouseDragDelta().x, ImGui::GetMouseDragDelta().y });
					runCallback(eventHandler->getMouseDragCallback(), m_activeWindow,
						mvPythonTranslator::ToPyMPair(i, 0));
					ImGui::ResetMouseDragDelta(i);
					break;
				}

				// reset, since event has already been dispatched
				mvInput::setMouseDragging(false);
				mvInput::setMouseDragDelta({ 0.0f, 0.0f });
			}
		}

		// route other mouse events
		for (int i = 0; i < IM_ARRAYSIZE(ImGui::GetIO().MouseDown); i++)
		{
			// route mouse click event
			if (ImGui::IsMouseClicked(i) && !eventHandler->getMouseClickCallback().empty())
				runCallback(eventHandler->getMouseClickCallback(), m_activeWindow,
					mvPythonTranslator::ToPyInt(i));

			// route mouse down event
			if (ImGui::GetIO().MouseDownDuration[i] >= 0.0f && !eventHandler->getMouseDownCallback().empty())
				runCallback(eventHandler->getMouseDownCallback(), m_activeWindow,
					mvPythonTranslator::ToPyMPair(i, ImGui::GetIO().MouseDownDuration[i]));

			// route mouse double clicked event
			if (ImGui::IsMouseDoubleClicked(i) && !eventHandler->getMouseDoubleClickCallback().empty())
				runCallback(eventHandler->getMouseDoubleClickCallback(), m_activeWindow,
					mvPythonTranslator::ToPyInt(i));

			// route mouse released event
			if (ImGui::IsMouseReleased(i) && !eventHandler->getMouseReleaseCallback().empty())
				runCallback(eventHandler->getMouseReleaseCallback(), m_activeWindow,
					mvPythonTranslator::ToPyInt(i));
		}
	}

	void mvApp::addRuntimeItem(const std::string& parent, const std::string& before, mvAppItem* item) 
	{ 
		if (!checkIfMainThread())
			return;

		m_newItemVec.push_back({ item, before, parent });
	}

	void mvApp::addMTCallback(const std::string& name, PyObject* data, const std::string& returnname) 
	{ 
		Py_XINCREF(data);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_asyncCallbacks.push_back({ name, data, returnname }); 
	}

	void mvApp::pushParent(mvAppItem* item)
	{
		m_parents.push(item);
	}

	mvAppItem* mvApp::popParent()
	{
		if (m_parents.empty())
		{
			ThrowPythonException("No parent to pop.");
			return nullptr;
		}

		mvAppItem* item = m_parents.top();
		m_parents.pop();
		return item;
	}

	mvAppItem* mvApp::topParent()
	{
		if(m_parents.size() != 0)
			return m_parents.top();
		return nullptr;
	}

	void mvApp::addItemColorStyle(const std::string& name, ImGuiCol item, mvColor color)
	{
		auto aitem = getItem(name);
		if (aitem)
			aitem->addColorStyle(item, color);
	}

	mvAppItem* mvApp::getItem(const std::string& name, bool ignoreRuntime)
	{

		if (!checkIfMainThread())
			return nullptr;

		mvAppItem* item = nullptr;

		if (!ignoreRuntime)
			item = getRuntimeItem(name);

		if (item)
			return item;

		for (auto window : m_windows)
		{
			if (window->getName() == name)
				return window;

			auto child = window->getChild(name);
			if (child)
				return child;
		}

		return nullptr;
	}

	mvAppItem* mvApp::getRuntimeItem(const std::string& name)
	{
		if (!checkIfMainThread())
			return nullptr;

		for (auto& item : m_newItemVec)
		{

			if (item.item->getName() == name)
				return item.item;
		}

		return nullptr;
	}

	mvWindowAppitem* mvApp::getWindow(const std::string& name)
	{

		if (!checkIfMainThread())
			return nullptr;

		mvAppItem* item = getRuntimeItem(name);
		if (item == nullptr)
			item = getItem(name);
		if (item == nullptr)
			return nullptr;

		if (item->getType() == mvAppItemType::Window)
			return static_cast<mvWindowAppitem*>(item);

		return nullptr;
	}

	void mvApp::runAsyncCallback(std::string name, PyObject* data, std::string returnname)
	{
		if (name.empty())
		{
			Py_XDECREF(data);
			return;
		}

		mvGlobalIntepreterLock gil;

		PyObject* pHandler = PyDict_GetItemString(PyModule_GetDict(PyImport_AddModule("__main__")), name.c_str()); // borrowed
		if (pHandler == NULL)
		{
			PyObject* pModules = PyDict_Values(PyImport_GetModuleDict());
			PyObject* pModulesKeys = PyDict_Keys(PyImport_GetModuleDict());
			for (int i = 0; i < PyList_Size(pModules); i++)
			{
				//mvAppLog::LogError(_PyUnicode_AsString(PyList_GetItem(pModulesKeys, i)));
				PyObject* pModule = PyModule_GetDict(PyList_GetItem(pModules, i));
				if (PyDict_Check(pModule))
				{
					pHandler = PyDict_GetItemString(pModule, name.c_str()); // borrowed reference
					if (pHandler)
						break;
				}

			}
			Py_XDECREF(pModules);
			Py_XDECREF(pModulesKeys);
		}

		// if callback doesn't exist
		if (pHandler == NULL)
		{
			std::string message(" Callback doesn't exist");
			ThrowPythonException(name + message);
			
			return;
		}

		// check if handler is callable
		if (PyCallable_Check(pHandler))
		{
			PyErr_Clear();

			PyObject* pArgs = PyTuple_New(2);
			PyTuple_SetItem(pArgs, 0, PyUnicode_FromString("Async"));
			PyTuple_SetItem(pArgs, 1, data); // steals data, so don't deref

			PyObject* result = PyObject_CallObject(pHandler, pArgs);

			// check if call succeded
			if (!result)
			{
				std::string message("Callback failed");
				ThrowPythonException(name + message);
			}

			if (!returnname.empty())
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_asyncReturns.push({ returnname, result });
			}
			else
				Py_XDECREF(result);

			Py_XDECREF(pArgs);
			
			// check if error occurred
			if (PyErr_Occurred())
				PyErr_Print();

		}

		else
		{
			std::string message(" Callback not callable");
			ThrowPythonException(name + message);
		}

	}

	void mvApp::runReturnCallback(const std::string& name, const std::string& sender, PyObject* data)
	{
		if (name.empty())
		{
			if (data != nullptr)
				Py_XDECREF(data);
			return;
		}

		if (data == nullptr)
		{
			data = Py_None;
			Py_XINCREF(data);
		}

		runCallback(name, sender, data);
	}

	void mvApp::runCallback(const std::string& name, const std::string& sender, PyObject* data)
	{
		if (name.empty())
		{
			if(data != nullptr)
				Py_XDECREF(data);
			return;
		}

		if (data == nullptr)
		{
			data = Py_None;
			Py_XINCREF(data);
		}

		mvGlobalIntepreterLock gil;

		PyObject* pHandler = PyDict_GetItemString(PyModule_GetDict(PyImport_AddModule("__main__")), name.c_str()); // borrowed
		if (pHandler == NULL)
		{
			PyObject* pModules = PyDict_Values(PyImport_GetModuleDict());
			PyObject* pModulesKeys = PyDict_Keys(PyImport_GetModuleDict());
			for (int i = 0; i < PyList_Size(pModules); i++)
			{
				PyObject* pModule = PyModule_GetDict(PyList_GetItem(pModules, i));
				if (PyDict_Check(pModule))
				{
					pHandler = PyDict_GetItemString(pModule, name.c_str()); // borrowed reference
					if (pHandler)
						break;
				}

			}
			Py_XDECREF(pModules);
			Py_XDECREF(pModulesKeys);
		}

		// if callback doesn't exist
		if (pHandler == NULL)
		{
			std::string message(" Callback doesn't exist");
			ThrowPythonException(name + message);
			return;
		}

		// check if handler is callable
		if (PyCallable_Check(pHandler))
		{

			PyErr_Clear();

			PyObject* pArgs = PyTuple_New(2);
			PyTuple_SetItem(pArgs, 0, PyUnicode_FromString(sender.c_str()));
			PyTuple_SetItem(pArgs, 1, data); // steals data, so don't deref

			PyObject* result = PyObject_CallObject(pHandler, pArgs);

			// check if call succeded
			if (!result)
			{
				std::string message("Callback failed");
				ThrowPythonException(name + message);
			}

			Py_XDECREF(pArgs);
			Py_XDECREF(result);

			// check if error occurred
			if (PyErr_Occurred())
				PyErr_Print();

		}

		else
		{
			std::string message(" Callback not callable");
			ThrowPythonException(name + message);
		}

	}

	void mvApp::setAppTheme(const std::string& theme)
	{
		m_theme = theme;

		if (s_started)
			changeTheme();
		
		// if app hasn't started, defer theme setting
		else
			m_compileTimeThemeSet = true;

		m_styleChange = true;

	}

	void mvApp::changeTheme()
	{

		// TODO: All themes borrowed from online need to be
		//       cleaned up for consistency and completed.

		if (m_theme == "Dark")
			ImGui::StyleColorsDark(&m_newstyle);

		else if (m_theme == "Classic")
			ImGui::StyleColorsClassic(&m_newstyle);

		else if (m_theme == "Light")
			ImGui::StyleColorsLight(&m_newstyle);

		else if (m_theme == "Dark 2")
		{
			m_newstyle.FrameRounding = 2.3f;
			m_newstyle.ScrollbarRounding = 0;
			m_newstyle.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.90f, 0.90f);
			m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
			m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.10f, 0.85f);
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.70f, 0.70f, 0.70f, 0.65f);
			m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.00f, 0.00f, 0.01f, 1.00f);
			m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.80f, 0.80f, 0.40f);
			m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.65f, 0.65f, 0.45f);
			m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.83f);
			m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.40f, 0.40f, 0.80f, 0.20f);
			m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.00f, 0.00f, 0.87f);
			m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.01f, 0.01f, 0.02f, 0.80f);
			m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.20f, 0.25f, 0.30f, 0.60f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.55f, 0.53f, 0.55f, 0.51f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.56f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.91f);
			m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.90f, 0.90f, 0.83f);
			m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.70f, 0.70f, 0.70f, 0.62f);
			m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.30f, 0.30f, 0.84f);
			m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.48f, 0.72f, 0.89f, 0.49f);
			m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.69f, 0.99f, 0.68f);
			m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.50f, 0.50f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.30f, 0.69f, 1.00f, 0.53f);
			m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.44f, 0.61f, 0.86f, 1.00f);
			m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.38f, 0.62f, 0.83f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Separator] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Tab] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabHovered] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabActive] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.0f, 1.0f, 0.0f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.85f);
			m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
			m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
			m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 1.00f, 0.35f);
			m_newstyle.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
		}

		else if (m_theme == "Dark Grey")
		{
			m_newstyle.Colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
			m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
			m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
			m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.21f, 0.22f, 0.54f);
			m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.40f, 0.40f, 0.40f);
			m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.18f, 0.18f, 0.67f);
			m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
			m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
			m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.44f, 0.44f, 0.44f, 0.40f);
			m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.46f, 0.47f, 0.48f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.70f, 0.70f, 0.70f, 0.31f);
			m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.70f, 0.70f, 0.80f);
			m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.48f, 0.50f, 0.52f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
			m_newstyle.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.72f, 0.72f, 0.72f, 0.78f);
			m_newstyle.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.91f, 0.91f, 0.91f, 0.25f);
			m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.81f, 0.81f, 0.81f, 0.67f);
			m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.46f, 0.46f, 0.46f, 0.95f);
			m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.73f, 0.60f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.87f, 0.87f, 0.87f, 0.35f);
			m_newstyle.Colors[ImGuiCol_ModalWindowDarkening] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
			m_newstyle.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
			m_newstyle.Colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
			m_newstyle.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		}

		else if (m_theme == "Cherry")
		{
			m_newstyle.Colors[ImGuiCol_Text] = ImVec4(0.860f, 0.930f, 0.890f, 0.78f);
			m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.860f, 0.930f, 0.890f, 0.28f);
			m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(0.200f, 0.220f, 0.270f, 0.58f);
			m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.200f, 0.220f, 0.270f, 0.9f);
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.31f, 0.31f, 1.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.200f, 0.220f, 0.270f, 1.00f);
			m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.455f, 0.198f, 0.301f, 0.78f);
			m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.232f, 0.201f, 0.271f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.502f, 0.075f, 0.256f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.200f, 0.220f, 0.270f, 0.75f);
			m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.200f, 0.220f, 0.270f, 0.47f);
			m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.200f, 0.220f, 0.270f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.09f, 0.15f, 0.16f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.455f, 0.198f, 0.301f, 0.78f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(0.71f, 0.22f, 0.27f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
			m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.71f, 0.22f, 0.27f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
			m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.455f, 0.198f, 0.301f, 0.86f);
			m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.455f, 0.198f, 0.301f, 0.76f);
			m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.455f, 0.198f, 0.301f, 0.86f);
			m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.502f, 0.075f, 0.256f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.47f, 0.77f, 0.83f, 0.04f);
			m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.455f, 0.198f, 0.301f, 0.78f);
			m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(0.860f, 0.930f, 0.890f, 0.63f);
			m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.860f, 0.930f, 0.890f, 0.63f);
			m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.455f, 0.198f, 0.301f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.455f, 0.198f, 0.301f, 0.43f);
			m_newstyle.WindowPadding = ImVec2(6, 4);
			m_newstyle.WindowRounding = 0.0f;
			m_newstyle.FramePadding = ImVec2(5, 2);
			m_newstyle.FrameRounding = 3.0f;
			m_newstyle.ItemSpacing = ImVec2(7, 1);
			m_newstyle.ItemInnerSpacing = ImVec2(1, 1);
			m_newstyle.TouchExtraPadding = ImVec2(0, 0);
			m_newstyle.IndentSpacing = 6.0f;
			m_newstyle.ScrollbarSize = 12.0f;
			m_newstyle.ScrollbarRounding = 16.0f;
			m_newstyle.GrabMinSize = 20.0f;
			m_newstyle.GrabRounding = 2.0f;
			m_newstyle.WindowTitleAlign.x = 0.50f;
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.539f, 0.479f, 0.255f, 0.162f);
			m_newstyle.FrameBorderSize = 0.0f;
			m_newstyle.WindowBorderSize = 1.0f;
		}

		else if (m_theme == "Grey")
		{

		/// 0 = FLAT APPEARENCE
		/// 1 = MORE "3D" LOOK
		int is3D = 0;

		m_newstyle.Colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
		m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.12f, 0.12f, 0.12f, 0.71f);
		m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
		m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.42f, 0.42f, 0.42f, 0.54f);
		m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.42f, 0.42f, 0.42f, 0.40f);
		m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.56f, 0.56f, 0.56f, 0.67f);
		m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.17f, 0.17f, 0.17f, 0.90f);
		m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.335f, 0.335f, 0.335f, 1.000f);
		m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.24f, 0.24f, 0.24f, 0.53f);
		m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
		m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
		m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
		m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
		m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.52f, 0.52f, 0.52f, 1.00f);
		m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.64f, 0.64f, 0.64f, 1.00f);
		m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.54f, 0.54f, 0.54f, 0.35f);
		m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.52f, 0.52f, 0.59f);
		m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.76f, 0.76f, 0.76f, 1.00f);
		m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.38f, 0.38f, 0.38f, 1.00f);
		m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
		m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.76f, 0.76f, 0.76f, 0.77f);
		m_newstyle.Colors[ImGuiCol_Separator] = ImVec4(0.000f, 0.000f, 0.000f, 0.137f);
		m_newstyle.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.700f, 0.671f, 0.600f, 0.290f);
		m_newstyle.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.702f, 0.671f, 0.600f, 0.674f);
		m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.25f);
		m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
		m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
		m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
		m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
		m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
		m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.73f, 0.73f, 0.73f, 0.35f);
		m_newstyle.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
		m_newstyle.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
		m_newstyle.Colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
		m_newstyle.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
		m_newstyle.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
		m_newstyle.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TabActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
		m_newstyle.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
		m_newstyle.PopupRounding = 3;
		m_newstyle.WindowPadding = ImVec2(4, 4);
		m_newstyle.FramePadding = ImVec2(6, 4);
		m_newstyle.ItemSpacing = ImVec2(6, 2);
		m_newstyle.ScrollbarSize = 18;
		m_newstyle.TabBorderSize = is3D;
		m_newstyle.TabRounding = 3;
		m_newstyle.WindowBorderSize = 1;
		m_newstyle.ChildBorderSize = 1;
		m_newstyle.PopupBorderSize = 1;
		m_newstyle.FrameBorderSize = is3D;
		m_newstyle.WindowRounding = 3;
		m_newstyle.ChildRounding = 3;
		m_newstyle.FrameRounding = 3;
		m_newstyle.ScrollbarRounding = 2;
		m_newstyle.GrabRounding = 3;
		}

		else if (m_theme == "Purple")
		{
		EditorColorScheme::SetColors(0x1F2421FF, 0xDCE1DEFF, 0x725AC1FF, 0x8D86C9FF, 0xECA400FF);
		EditorColorScheme::ApplyTheme(m_newstyle);
		}

		else if (m_theme == "Gold")
		{

			m_newstyle.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.44f, 0.44f, 0.44f, 1.00f);
			m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
			m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.00f, 0.00f, 0.51f);
			m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.53f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.47f, 0.47f, 0.47f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.81f, 0.83f, 0.81f, 1.00f);
			m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.93f, 0.65f, 0.14f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Separator] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Tab] = ImVec4(0.51f, 0.36f, 0.15f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabHovered] = ImVec4(0.91f, 0.64f, 0.13f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabActive] = ImVec4(0.78f, 0.55f, 0.21f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.07f, 0.10f, 0.15f, 0.97f);
			m_newstyle.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
			m_newstyle.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
			m_newstyle.Colors[ImGuiCol_NavHighlight] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
			m_newstyle.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
			m_newstyle.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
			m_newstyle.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
			m_newstyle.FramePadding = ImVec2(4, 2);
			m_newstyle.ItemSpacing = ImVec2(10, 2);
			m_newstyle.IndentSpacing = 12;
			m_newstyle.ScrollbarSize = 10;
			m_newstyle.WindowRounding = 4;
			m_newstyle.FrameRounding = 4;
			m_newstyle.PopupRounding = 4;
			m_newstyle.ScrollbarRounding = 6;
			m_newstyle.GrabRounding = 4;
			m_newstyle.TabRounding = 4;
			m_newstyle.WindowTitleAlign = ImVec2(1.0f, 0.5f);
			m_newstyle.WindowMenuButtonPosition = ImGuiDir_Right;
			m_newstyle.DisplaySafeAreaPadding = ImVec2(4, 4);
		}

		else if (m_theme == "Red")
		{
			m_newstyle.FrameRounding = 4.0f;
			m_newstyle.WindowBorderSize = 0.0f;
			m_newstyle.PopupBorderSize = 0.0f;
			m_newstyle.GrabRounding = 4.0f;

			m_newstyle.Colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextDisabled] = ImVec4(0.73f, 0.75f, 0.74f, 1.00f);
			m_newstyle.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.09f, 0.94f);
			m_newstyle.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
			m_newstyle.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
			m_newstyle.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			m_newstyle.Colors[ImGuiCol_FrameBg] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.84f, 0.66f, 0.66f, 0.40f);
			m_newstyle.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.84f, 0.66f, 0.66f, 0.67f);
			m_newstyle.Colors[ImGuiCol_TitleBg] = ImVec4(0.47f, 0.22f, 0.22f, 0.67f);
			m_newstyle.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.47f, 0.22f, 0.22f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.47f, 0.22f, 0.22f, 0.67f);
			m_newstyle.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.34f, 0.16f, 0.16f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
			m_newstyle.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
			m_newstyle.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrab] = ImVec4(0.71f, 0.39f, 0.39f, 1.00f);
			m_newstyle.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.84f, 0.66f, 0.66f, 1.00f);
			m_newstyle.Colors[ImGuiCol_Button] = ImVec4(0.47f, 0.22f, 0.22f, 0.65f);
			m_newstyle.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.71f, 0.39f, 0.39f, 0.65f);
			m_newstyle.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.20f, 0.20f, 0.50f);
			m_newstyle.Colors[ImGuiCol_Header] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.84f, 0.66f, 0.66f, 0.65f);
			m_newstyle.Colors[ImGuiCol_HeaderActive] = ImVec4(0.84f, 0.66f, 0.66f, 0.00f);
			m_newstyle.Colors[ImGuiCol_Separator] = ImVec4(0.43f, 0.43f, 0.50f, 0.50f);
			m_newstyle.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.84f, 0.66f, 0.66f, 0.66f);
			m_newstyle.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.84f, 0.66f, 0.66f, 0.66f);
			m_newstyle.Colors[ImGuiCol_Tab] = ImVec4(0.71f, 0.39f, 0.39f, 0.54f);
			m_newstyle.Colors[ImGuiCol_TabHovered] = ImVec4(0.84f, 0.66f, 0.66f, 0.66f);
			m_newstyle.Colors[ImGuiCol_TabActive] = ImVec4(0.84f, 0.66f, 0.66f, 0.66f);
			m_newstyle.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.07f, 0.10f, 0.15f, 0.97f);
			m_newstyle.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.26f, 0.42f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLines] = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
			m_newstyle.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
			m_newstyle.Colors[ImGuiCol_DragDropTarget] = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
			m_newstyle.Colors[ImGuiCol_NavHighlight] = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
			m_newstyle.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
			m_newstyle.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
			m_newstyle.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
		}

		else
			ImGui::StyleColorsDark();
	}

	void mvApp::setThemeItem(long item, mvColor color)
	{
		m_newstyle.Colors[item] = ImVec4(color.r/255.0f, color.g/255.0f, color.b/255.0f, color.a/255.0f);
		m_styleChange = true;
	}

	mvColor mvApp::getThemeItem(long item)
	{
		ImGuiStyle* style = &ImGui::GetStyle();
		mvColor color = {(int)style->Colors[item].x * 255, (int)style->Colors[item].y * 255 ,
			(int)style->Colors[item].z * 255 , (int)style->Colors[item].w * 255 };
		return color;
	}

	void mvApp::addItem(mvAppItem* item)
	{
		if (!checkIfMainThread())
			return;

		static int count = 0;
		count++;

		assert(!IsAppStarted()); // should not be callable during runtime

		if (!item->areDuplicatesAllowed())
		{
			if (getItem(item->getName()))
			{
				std::string message = item->getName() + " " + std::to_string(count);
				ThrowPythonException(message + ": Items of this type must have unique names");
				return;
			}
		}

		mvAppItem* parentitem = topParent();	
		if (parentitem == nullptr)
			parentitem = m_windows[0];

		item->setParent(parentitem);
		parentitem->addChild(item);
	}

	void mvApp::addWindow(mvAppItem* item)
	{
		if (!checkIfMainThread())
			return;

		m_windows.push_back(item);
	}

}