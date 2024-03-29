#include <iostream>
#include <queue>
#include "Config.h"
#include "Format.h"
#include "GameController.h"
#include "GameModel.h"

#include "simulation/Simulation.h"
#include "GameView.h"
#include "graphics/Graphics.h"
#include "simulation/SaveRenderer.h"
#include "simulation/SimulationData.h"

#include "client/SaveInfo.h"
#include "client/GameSave.h"
#include "gui/search/SearchController.h"
#include "gui/render/RenderController.h"
#include "gui/login/LoginController.h"
#include "gui/interface/Point.h"
#include "gui/dialogues/ErrorMessage.h"
#include "gui/dialogues/InformationMessage.h"
#include "gui/dialogues/ConfirmPrompt.h"
#include "GameModelException.h"
#include "simulation/Air.h"
#include "gui/elementsearch/ElementSearchActivity.h"
#include "gui/profile/ProfileActivity.h"
#include "gui/colourpicker/ColourPickerActivity.h"
#include "gui/update/UpdateActivity.h"
#include "Notification.h"
#include "gui/filebrowser/FileBrowserActivity.h"
#include "gui/save/LocalSaveActivity.h"
#include "gui/save/ServerSaveActivity.h"
#include "gui/interface/Keys.h"
#include "simulation/Snapshot.h"
#include "debug/DebugInfo.h"
#include "debug/DebugParts.h"
#include "debug/ElementPopulation.h"
#include "debug/DebugLines.h"
#ifdef LUACONSOLE
#include "lua/LuaScriptInterface.h"
#else
#include "lua/TPTScriptInterface.h"
#endif

using namespace std;

class GameController::SearchCallback: public ControllerCallback
{
	GameController * cc;
public:
	SearchCallback(GameController * cc_) { cc = cc_; }
	virtual void ControllerExit()
	{
		if(cc->search->GetLoadedSave())
		{
			try
			{
				cc->gameModel->SetSave(cc->search->GetLoadedSave());
				cc->search->ReleaseLoadedSave();
			}
			catch(GameModelException & ex)
			{
				new ErrorMessage("Cannot open save", ex.what());
			}
		}
	}
};

class GameController::SaveOpenCallback: public ControllerCallback
{
	GameController * cc;
public:
	SaveOpenCallback(GameController * cc_) { cc = cc_; }
	virtual void ControllerExit()
	{
		if(cc->activePreview->GetDoOpen() && cc->activePreview->GetSave())
		{
			try
			{
				cc->LoadSave(cc->activePreview->GetSave());
			}
			catch(GameModelException & ex)
			{
				new ErrorMessage("Cannot open save", ex.what());
			}
		}
	}
};

class GameController::OptionsCallback: public ControllerCallback
{
	GameController * cc;
public:
	OptionsCallback(GameController * cc_) { cc = cc_; }
	virtual void ControllerExit()
	{
		cc->gameModel->UpdateQuickOptions();
	}
};

class GameController::TagsCallback: public ControllerCallback
{
	GameController * cc;
public:
	TagsCallback(GameController * cc_) { cc = cc_; }
	virtual void ControllerExit()
	{
		cc->gameView->NotifySaveChanged(cc->gameModel);
	}
};

class GameController::StampsCallback: public ControllerCallback
{
	GameController * cc;
public:
	StampsCallback(GameController * cc_) { cc = cc_; }
	virtual void ControllerExit()
	{
		if(cc->localBrowser->GetSave())
		{
			if (cc->localBrowser->GetMoveToFront())
				Client::Ref().MoveStampToFront(cc->localBrowser->GetSave()->GetName());
			cc->LoadStamp(cc->localBrowser->GetSave()->GetGameSave());
		}
	}
};

GameController::GameController():
	firstTick(true),
	foundSign(NULL),
	activePreview(NULL),
	search(NULL),
	renderOptions(NULL),
	loginWindow(NULL),
	console(NULL),
	tagsWindow(NULL),
	localBrowser(NULL),
	options(NULL),
	debugFlags(0),
	HasDone(false)
{
	gameView = new GameView();
	simulation = new Simulation();
	gameModel = new GameModel();
	gameModel->BuildQuickOptionMenu(this);

	gameView->AttachController(this);
	gameModel->AddObserver(gameView);

	gameView->SetDebugHUD(Client::Ref().GetPrefBool("Renderer.DebugMode", false));

#ifdef LUACONSOLE
	commandInterface = new LuaScriptInterface(this, gameModel);
	((LuaScriptInterface*)commandInterface)->SetWindow(gameView);
#else
	commandInterface = new TPTScriptInterface(this, gameModel);
#endif

	commandInterface->OnBrushChanged(gameModel->GetBrushID(), gameModel->GetBrush()->GetRadius().X, gameModel->GetBrush()->GetRadius().X);
	ActiveToolChanged(0, gameModel->GetActiveTool(0));
	ActiveToolChanged(1, gameModel->GetActiveTool(1));
	ActiveToolChanged(2, gameModel->GetActiveTool(2));
	ActiveToolChanged(3, gameModel->GetActiveTool(3));

	Client::Ref().AddListener(this);

	debugInfo.push_back(new DebugParts(0x1, gameModel->GetSimulation()));
	debugInfo.push_back(new ElementPopulationDebug(0x2, gameModel->GetSimulation()));
	debugInfo.push_back(new DebugLines(0x4, gameView, this));
}

GameController::~GameController()
{
	if(search)
	{
		delete search;
	}
	if(renderOptions)
	{
		delete renderOptions;
	}
	if(loginWindow)
	{
		delete loginWindow;
	}
	if(tagsWindow)
	{
		delete tagsWindow;
	}
	if(console)
	{
		delete console;
	}
	if(activePreview)
	{
		delete activePreview;
	}
	if(localBrowser)
	{
		delete localBrowser;
	}
	if (options)
	{
		delete options;
	}
	//deleted here because it refuses to be deleted when deleted from gameModel even with the same code
	std::deque<Snapshot*> history = gameModel->GetHistory();
	for(std::deque<Snapshot*>::iterator iter = history.begin(), end = history.end(); iter != end; ++iter)
	{
		delete *iter;
	}
	std::vector<QuickOption*> quickOptions = gameModel->GetQuickOptions();
	for(std::vector<QuickOption*>::iterator iter = quickOptions.begin(), end = quickOptions.end(); iter != end; ++iter)
	{
		delete *iter;
	}
	std::vector<Notification*> notifications = gameModel->GetNotifications();
	for(std::vector<Notification*>::iterator iter = notifications.begin(); iter != notifications.end(); ++iter)
	{
		delete *iter;
	}
	delete gameModel;
	if (ui::Engine::Ref().GetWindow() == gameView)
	{
		ui::Engine::Ref().CloseWindow();
		delete gameView;
	}
}

void GameController::HistoryRestore()
{
	std::deque<Snapshot*> history = gameModel->GetHistory();
	if(history.size())
	{
		Snapshot * snap = history.back();
		gameModel->GetSimulation()->Restore(*snap);
		if(history.size()>1)
		{
			history.pop_back();
			delete snap;
			gameModel->SetHistory(history);
		}
	}
}

void GameController::HistorySnapshot()
{
	std::deque<Snapshot*> history = gameModel->GetHistory();
	Snapshot * newSnap = gameModel->GetSimulation()->CreateSnapshot();
	if(newSnap)
	{
		if(history.size() >= 1) //History limit is current 1
		{
			Snapshot * snap = history.front();
			history.pop_front();
			//snap->Particles.clear();
			delete snap;
		}
		history.push_back(newSnap);
		gameModel->SetHistory(history);
	}
}

GameView * GameController::GetView()
{
	return gameView;
}

sign * GameController::GetSignAt(int x, int y)
{
	Simulation * sim = gameModel->GetSimulation();
	for (std::vector<sign>::reverse_iterator iter = sim->signs.rbegin(), end = sim->signs.rend(); iter != end; ++iter)
	{
		int signx, signy, signw, signh;
		(*iter).pos((*iter).getText(sim), signx, signy, signw, signh);
		if (x>=signx && x<=signx+signw && y>=signy && y<=signy+signh)
			return &(*iter);
	}
	return NULL;
}

void GameController::PlaceSave(ui::Point position)
{
	if(gameModel->GetPlaceSave())
	{
		gameModel->GetSimulation()->Load(position.X, position.Y, gameModel->GetPlaceSave());
		gameModel->SetPaused(gameModel->GetPlaceSave()->paused | gameModel->GetPaused());
		HistorySnapshot();
	}
}

void GameController::Install()
{
#if defined(MACOSX)
	new InformationMessage("No Installation necessary", "You don't need to install The Powder Toy on Mac OS X", false);
#elif defined(WIN) || defined(LIN)
	class InstallConfirmation: public ConfirmDialogueCallback {
	public:
		GameController * c;
		InstallConfirmation(GameController * c_) {	c = c_;	}
		virtual void ConfirmCallback(ConfirmPrompt::DialogueResult result) {
			if (result == ConfirmPrompt::ResultOkay)
			{
				if(Client::Ref().DoInstallation())
				{
					new InformationMessage("Install Success", "The installation completed!", false);
				}
				else
				{
					new ErrorMessage("Could not install", "The installation did not complete due to an error");
				}
			}
		}
		virtual ~InstallConfirmation() { }
	};
	new ConfirmPrompt("Install The Powder Toy", "Do you wish to install The Powder Toy on this computer?\nThis allows you to open save files and saves directly from the website.", new InstallConfirmation(this));
#else
	new ErrorMessage("Cannot install", "You cannot install The Powder Toy on this platform");
#endif
}

void GameController::AdjustGridSize(int direction)
{
	if(direction > 0)
		gameModel->GetRenderer()->SetGridSize((gameModel->GetRenderer()->GetGridSize()+1)%10);
	else
		gameModel->GetRenderer()->SetGridSize((gameModel->GetRenderer()->GetGridSize()+9)%10);
}

void GameController::InvertAirSim()
{
	gameModel->GetSimulation()->air->Invert();
}


void GameController::AdjustBrushSize(int direction, bool logarithmic, bool xAxis, bool yAxis)
{
	if(xAxis && yAxis)
		return;

	ui::Point newSize(0, 0);
	ui::Point oldSize = gameModel->GetBrush()->GetRadius();
	if(logarithmic)
		newSize = gameModel->GetBrush()->GetRadius() + ui::Point(direction * ((gameModel->GetBrush()->GetRadius().X/5)>0?gameModel->GetBrush()->GetRadius().X/5:1), direction * ((gameModel->GetBrush()->GetRadius().Y/5)>0?gameModel->GetBrush()->GetRadius().Y/5:1));
	else
		newSize = gameModel->GetBrush()->GetRadius() + ui::Point(direction, direction);
	if(newSize.X < 0)
		newSize.X = 0;
	if(newSize.Y < 0)
		newSize.Y = 0;
	if(newSize.X > 200)
		newSize.X = 200;
	if(newSize.Y > 200)
		newSize.Y = 200;

	if(xAxis)
		gameModel->GetBrush()->SetRadius(ui::Point(newSize.X, oldSize.Y));
	else if(yAxis)
		gameModel->GetBrush()->SetRadius(ui::Point(oldSize.X, newSize.Y));
	else
		gameModel->GetBrush()->SetRadius(newSize);

	BrushChanged(gameModel->GetBrushID(), gameModel->GetBrush()->GetRadius().X, gameModel->GetBrush()->GetRadius().Y);
}

void GameController::SetBrushSize(ui::Point newSize)
{
	gameModel->GetBrush()->SetRadius(newSize);
	BrushChanged(gameModel->GetBrushID(), gameModel->GetBrush()->GetRadius().X, gameModel->GetBrush()->GetRadius().Y);
}

void GameController::AdjustZoomSize(int direction, bool logarithmic)
{
	int newSize;
	if(logarithmic)
		newSize = gameModel->GetZoomSize()+(((gameModel->GetZoomSize()/10)>0?(gameModel->GetZoomSize()/10):1)*direction);
	else
		newSize = gameModel->GetZoomSize()+direction;
	if(newSize<5)
			newSize = 5;
	if(newSize>64)
			newSize = 64;
	gameModel->SetZoomSize(newSize);

	int newZoomFactor = 256/newSize;
	if(newZoomFactor<3)
		newZoomFactor = 3;
	gameModel->SetZoomFactor(newZoomFactor);
}

bool GameController::MouseInZoom(ui::Point position)
{
	if(position.X >= XRES)
		position.X = XRES-1;
	if(position.Y >= YRES)
		position.Y = YRES-1;
	if(position.Y < 0)
		position.Y = 0;
	if(position.X < 0)
		position.X = 0;

	return gameModel->MouseInZoom(position);
}

ui::Point GameController::PointTranslate(ui::Point point)
{
	if(point.X >= XRES)
		point.X = XRES-1;
	if(point.Y >= YRES)
		point.Y = YRES-1;
	if(point.Y < 0)
		point.Y = 0;
	if(point.X < 0)
		point.X = 0;

	return gameModel->AdjustZoomCoords(point);
}

ui::Point GameController::NormaliseBlockCoord(ui::Point point)
{
	return (point/CELL)*CELL;
}

void GameController::DrawRect(int toolSelection, ui::Point point1, ui::Point point2)
{
	Simulation * sim = gameModel->GetSimulation();
	Tool * activeTool = gameModel->GetActiveTool(toolSelection);
	gameModel->SetLastTool(activeTool);
	Brush * cBrush = gameModel->GetBrush();
	if(!activeTool || !cBrush)
		return;
	activeTool->SetStrength(gameModel->GetToolStrength());
	activeTool->DrawRect(sim, cBrush, point1, point2);
}

void GameController::DrawLine(int toolSelection, ui::Point point1, ui::Point point2)
{
	Simulation * sim = gameModel->GetSimulation();
	Tool * activeTool = gameModel->GetActiveTool(toolSelection);
	gameModel->SetLastTool(activeTool);
	Brush * cBrush = gameModel->GetBrush();
	if(!activeTool || !cBrush)
		return;
	activeTool->SetStrength(gameModel->GetToolStrength());
	activeTool->DrawLine(sim, cBrush, point1, point2);
}

void GameController::DrawFill(int toolSelection, ui::Point point)
{
	Simulation * sim = gameModel->GetSimulation();
	Tool * activeTool = gameModel->GetActiveTool(toolSelection);
	gameModel->SetLastTool(activeTool);
	Brush * cBrush = gameModel->GetBrush();
	if(!activeTool || !cBrush)
		return;
	activeTool->SetStrength(gameModel->GetToolStrength());
	activeTool->DrawFill(sim, cBrush, point);
}

void GameController::DrawPoints(int toolSelection, queue<ui::Point> & pointQueue)
{
	Simulation * sim = gameModel->GetSimulation();
	Tool * activeTool = gameModel->GetActiveTool(toolSelection);
	gameModel->SetLastTool(activeTool);
	Brush * cBrush = gameModel->GetBrush();
	if(!activeTool || !cBrush)
	{
		if(!pointQueue.empty())
		{
			while(!pointQueue.empty())
			{
				pointQueue.pop();
			}
		}
		return;
	}

	activeTool->SetStrength(gameModel->GetToolStrength());
	if(!pointQueue.empty())
	{
		ui::Point sPoint(0, 0);
		int size = pointQueue.size();
		bool first = true;
		while(!pointQueue.empty())
		{
			ui::Point fPoint = pointQueue.front();
			pointQueue.pop();
			if(size > 1)
			{
				if (!first)
				{
					activeTool->DrawLine(sim, cBrush, sPoint, fPoint, true);
				}
				first = false;
			}
			else
			{
				activeTool->Draw(sim, cBrush, fPoint);
			}
			sPoint = fPoint;
		}
	}
}

void GameController::LoadClipboard()
{
	gameModel->SetPlaceSave(gameModel->GetClipboard());
	if(gameModel->GetPlaceSave() && gameModel->GetPlaceSave()->Collapsed())
		gameModel->GetPlaceSave()->Expand();
}

void GameController::LoadStamp(GameSave *stamp)
{
	gameModel->SetPlaceSave(stamp);
	if(gameModel->GetPlaceSave() && gameModel->GetPlaceSave()->Collapsed())
		gameModel->GetPlaceSave()->Expand();
}

void GameController::TranslateSave(ui::Point point)
{
	matrix2d transform = m2d_identity;
	vector2d translate = v2d_new(point.X, point.Y);
	gameModel->GetPlaceSave()->Transform(transform, translate);
	gameModel->SetPlaceSave(gameModel->GetPlaceSave());
}

void GameController::TransformSave(matrix2d transform)
{
	vector2d translate = v2d_zero;
	gameModel->GetPlaceSave()->Transform(transform, translate);
	gameModel->SetPlaceSave(gameModel->GetPlaceSave());
}

void GameController::ToolClick(int toolSelection, ui::Point point)
{
	Simulation * sim = gameModel->GetSimulation();
	Tool * activeTool = gameModel->GetActiveTool(toolSelection);
	Brush * cBrush = gameModel->GetBrush();
	if(!activeTool || !cBrush)
		return;
	activeTool->Click(sim, cBrush, point);
}

std::string GameController::StampRegion(ui::Point point1, ui::Point point2)
{
	GameSave * newSave;
	newSave = gameModel->GetSimulation()->Save(point1.X, point1.Y, point2.X, point2.Y);
	if(newSave)
	{
		newSave->paused = gameModel->GetPaused();
		return Client::Ref().AddStamp(newSave);
	}
	else
	{
		new ErrorMessage("Could not create stamp", "Error generating save file");
		return "";
	}
}

void GameController::CopyRegion(ui::Point point1, ui::Point point2)
{
	GameSave * newSave;
	newSave = gameModel->GetSimulation()->Save(point1.X, point1.Y, point2.X, point2.Y);
	if(newSave)
	{
		newSave->paused = gameModel->GetPaused();
		gameModel->SetClipboard(newSave);
	}
}

void GameController::CutRegion(ui::Point point1, ui::Point point2)
{
	CopyRegion(point1, point2);
	gameModel->GetSimulation()->clear_area(point1.X, point1.Y, point2.X-point1.X, point2.Y-point1.Y);
}

bool GameController::MouseMove(int x, int y, int dx, int dy)
{
	return commandInterface->OnMouseMove(x, y, dx, dy);
}

bool GameController::BrushChanged(int brushType, int rx, int ry)
{
	return commandInterface->OnBrushChanged(brushType, rx, ry);
}

bool GameController::MouseDown(int x, int y, unsigned button)
{
	bool ret = commandInterface->OnMouseDown(x, y, button);
	if (ret && y<YRES && x<XRES && !gameView->GetPlacingSave() && !gameView->GetPlacingZoom())
	{
		ui::Point point = gameModel->AdjustZoomCoords(ui::Point(x, y));
		x = point.X;
		y = point.Y;
		if (!gameModel->GetActiveTool(0) || gameModel->GetActiveTool(0)->GetIdentifier() != "DEFAULT_UI_SIGN" || button != BUTTON_LEFT) //If it's not a sign tool or you are right/middle clicking
		{
			foundSign = GetSignAt(x, y);
			if(foundSign && sign::splitsign(foundSign->text.c_str()))
				return false;
		}
	}
	return ret;
}

bool GameController::MouseUp(int x, int y, unsigned button)
{
	bool ret = commandInterface->OnMouseUp(x, y, button);
	if (ret && foundSign && y<YRES && x<XRES && !gameView->GetPlacingSave())
	{
		ui::Point point = gameModel->AdjustZoomCoords(ui::Point(x, y));
		x = point.X;
		y = point.Y;
		if (!gameModel->GetActiveTool(0) || gameModel->GetActiveTool(0)->GetIdentifier() != "DEFAULT_UI_SIGN" || button != BUTTON_LEFT) //If it's not a sign tool or you are right/middle clicking
		{
			sign * foundSign = GetSignAt(x, y);
			if (foundSign)
			{
				const char* str = foundSign->text.c_str();
				char type;
				int pos = sign::splitsign(str, &type);
				if (pos)
				{
					ret = false;
					if (type == 'c' || type == 't' || type == 's')
					{
						char buff[256];
						strcpy(buff, str+3);
						buff[pos-3] = 0;
						switch (type)
						{
						case 'c':
						{
							int saveID = format::StringToNumber<int>(std::string(buff));
							if (saveID)
								OpenSavePreview(saveID, 0, false);
							break;
						}
						case 't':
						{
							// buff is already confirmed to be a number by sign::splitsign
							std::stringstream uri;
							uri << "http://powdertoy.co.uk/Discussions/Thread/View.html?Thread=" << buff;
							OpenURI(uri.str());
							break;
						}
						case 's':
							OpenSearch(buff);
							break;
						}
					}
					else if (type == 'b')
					{
						Simulation * sim = gameModel->GetSimulation();
						sim->create_part(-1, foundSign->x, foundSign->y, PT_SPRK);
					}
				}
			}
		}
	}
	foundSign = NULL;
	return ret;
}

bool GameController::MouseWheel(int x, int y, int d)
{
	return commandInterface->OnMouseWheel(x, y, d);
}

bool GameController::KeyPress(int key, Uint16 character, bool shift, bool ctrl, bool alt)
{
	bool ret = commandInterface->OnKeyPress(key, character, shift, ctrl, alt);
	if(ret)
	{
		Simulation * sim = gameModel->GetSimulation();
		if (key == KEY_RIGHT)
		{
			sim->player.comm = (int)(sim->player.comm)|0x02;  //Go right command
		}
		if (key == KEY_LEFT)
		{
			sim->player.comm = (int)(sim->player.comm)|0x01;  //Go left command
		}
		if (key == KEY_DOWN && ((int)(sim->player.comm)&0x08)!=0x08)
		{
			sim->player.comm = (int)(sim->player.comm)|0x08;  //Use element command
		}
		if (key == KEY_UP && ((int)(sim->player.comm)&0x04)!=0x04)
		{
			sim->player.comm = (int)(sim->player.comm)|0x04;  //Jump command
		}

		if (key == KEY_d)
		{
			sim->player2.comm = (int)(sim->player2.comm)|0x02;  //Go right command
		}
		if (key == KEY_a)
		{
			sim->player2.comm = (int)(sim->player2.comm)|0x01;  //Go left command
		}
		if (key == KEY_s && ((int)(sim->player2.comm)&0x08)!=0x08)
		{
			sim->player2.comm = (int)(sim->player2.comm)|0x08;  //Use element command
		}
		if (key == KEY_w && ((int)(sim->player2.comm)&0x04)!=0x04)
		{
			sim->player2.comm = (int)(sim->player2.comm)|0x04;  //Jump command
		}

		if((!sim->elementCount[PT_STKM2] || ctrl) && gameView->GetSelectMode() == SelectNone)
		{
			switch(key)
			{
			case 'w':
				SwitchGravity();
				break;
			case 'd':
				gameView->SetDebugHUD(!gameView->GetDebugHUD());
				break;
			case 's':
				gameView->BeginStampSelection();
				break;
			}
		}
	}
	return ret;
}

bool GameController::KeyRelease(int key, Uint16 character, bool shift, bool ctrl, bool alt)
{
	bool ret = commandInterface->OnKeyRelease(key, character, shift, ctrl, alt);
	if(ret)
	{
		Simulation * sim = gameModel->GetSimulation();
		if (key == KEY_RIGHT || key == KEY_LEFT)
		{
			sim->player.pcomm = sim->player.comm;  //Saving last movement
			sim->player.comm = (int)(sim->player.comm)&12;  //Stop command
		}
		if (key == KEY_UP)
		{
			sim->player.comm = (int)(sim->player.comm)&11;
		}
		if (key == KEY_DOWN)
		{
			sim->player.comm = (int)(sim->player.comm)&7;
		}

		if (key == KEY_d || key == KEY_a)
		{
			sim->player2.pcomm = sim->player2.comm;  //Saving last movement
			sim->player2.comm = (int)(sim->player2.comm)&12;  //Stop command
		}
		if (key == KEY_w)
		{
			sim->player2.comm = (int)(sim->player2.comm)&11;
		}
		if (key == KEY_s)
		{
			sim->player2.comm = (int)(sim->player2.comm)&7;
		}
	}
	return ret;
}

bool GameController::MouseTick()
{
	return commandInterface->OnMouseTick();
}

void GameController::Tick()
{
	if(firstTick)
	{
#ifdef LUACONSOLE
		((LuaScriptInterface*)commandInterface)->Init();
#endif
#if !defined(MACOSX) && !defined(NO_INSTALL_CHECK)
		if (Client::Ref().IsFirstRun())
		{
			Install();
		}
#endif
		firstTick = false;
	}
	for(std::vector<DebugInfo*>::iterator iter = debugInfo.begin(), end = debugInfo.end(); iter != end; iter++)
	{
		if ((*iter)->ID & debugFlags)
			(*iter)->Draw();
	}
	commandInterface->OnTick();
}

void GameController::Exit()
{
	if(ui::Engine::Ref().GetWindow() == gameView)
		ui::Engine::Ref().CloseWindow();
	HasDone = true;
}

void GameController::ResetAir()
{
	Simulation * sim = gameModel->GetSimulation();
	sim->air->Clear();
	for (int i = 0; i < NPART; i++)
	{
		if (sim->parts[i].type == PT_QRTZ || sim->parts[i].type == PT_GLAS || sim->parts[i].type == PT_TUNG)
		{
			sim->parts[i].pavg[0] = sim->parts[i].pavg[1] = 0;
		}
	}
}

void GameController::ResetSpark()
{
	Simulation * sim = gameModel->GetSimulation();
	for (int i = 0; i < NPART; i++)
		if (sim->parts[i].type == PT_SPRK)
		{
			if (sim->parts[i].ctype >= 0 && sim->parts[i].ctype < PT_NUM && sim->elements[sim->parts[i].ctype].Enabled)
			{
				sim->parts[i].type = sim->parts[i].ctype;
				sim->parts[i].ctype = sim->parts[i].life = 0;
			}
			else
				sim->kill_part(i);
		}
}

void GameController::SwitchGravity()
{
	gameModel->GetSimulation()->gravityMode = (gameModel->GetSimulation()->gravityMode+1)%3;

	switch (gameModel->GetSimulation()->gravityMode)
	{
	case 0:
		gameModel->SetInfoTip("Gravity: Vertical");
		break;
	case 1:
		gameModel->SetInfoTip("Gravity: Off");
		break;
	case 2:
		gameModel->SetInfoTip("Gravity: Radial");
		break;
	}
}

void GameController::SwitchAir()
{
	gameModel->GetSimulation()->air->airMode = (gameModel->GetSimulation()->air->airMode+1)%5;

	switch (gameModel->GetSimulation()->air->airMode)
	{
	case 0:
		gameModel->SetInfoTip("Air: On");
		break;
	case 1:
		gameModel->SetInfoTip("Air: Pressure Off");
		break;
	case 2:
		gameModel->SetInfoTip("Air: Velocity Off");
		break;
	case 3:
		gameModel->SetInfoTip("Air: Off");
		break;
	case 4:
		gameModel->SetInfoTip("Air: No Update");
		break;
	}
}

void GameController::ToggleAHeat()
{
	gameModel->SetAHeatEnable(!gameModel->GetAHeatEnable());
}

void GameController::ToggleNewtonianGravity()
{
	if (gameModel->GetSimulation()->grav->ngrav_enable)
		gameModel->GetSimulation()->grav->stop_grav_async();
	else
		gameModel->GetSimulation()->grav->start_grav_async();
	gameModel->UpdateQuickOptions();
}


void GameController::LoadRenderPreset(int presetNum)
{
	Renderer * renderer = gameModel->GetRenderer();
	RenderPreset preset = renderer->renderModePresets[presetNum];
	gameModel->SetInfoTip(preset.Name);
	renderer->SetRenderMode(preset.RenderModes);
	renderer->SetDisplayMode(preset.DisplayModes);
	renderer->SetColourMode(preset.ColourMode);
}

void GameController::Update()
{
	ui::Point pos = gameView->GetMousePosition();
	gameModel->GetRenderer()->mousePos = PointTranslate(pos);
	if (pos.X < XRES && pos.Y < YRES)
		gameView->SetSample(gameModel->GetSimulation()->GetSample(PointTranslate(pos).X, PointTranslate(pos).Y));
	else
		gameView->SetSample(gameModel->GetSimulation()->GetSample(pos.X, pos.Y));

	Simulation * sim = gameModel->GetSimulation();
	sim->UpdateSim();
	if (!sim->sys_pause || sim->framerender)
		sim->UpdateParticles(0, NPART);

	//if either STKM or STK2 isn't out, reset it's selected element. Defaults to PT_DUST unless right selected is something else
	//This won't run if the stickmen dies in a frame, since it respawns instantly
	if (!sim->player.spwn || !sim->player2.spwn)
	{
		int rightSelected = PT_DUST;
		Tool * activeTool = gameModel->GetActiveTool(1);
		if (activeTool->GetIdentifier().find("DEFAULT_PT_") != activeTool->GetIdentifier().npos)
		{
			int sr = activeTool->GetToolID();
			if ((sr>0 && sr<PT_NUM && sim->elements[sr].Enabled && sim->elements[sr].Falldown>0) || sr==SPC_AIR || sr == PT_NEUT || sr == PT_PHOT || sr == PT_LIGH)
				rightSelected = sr;
		}

		if (!sim->player.spwn)
			sim->player.elem = rightSelected;
		if (!sim->player2.spwn)
			sim->player2.elem = rightSelected;
	}
	if(renderOptions && renderOptions->HasExited)
	{
		delete renderOptions;
		renderOptions = NULL;
	}

	if(search && search->HasExited)
	{
		delete search;
		search = NULL;
	}

	if(activePreview && activePreview->HasExited)
	{
		delete activePreview;
		activePreview = NULL;
	}

	if(loginWindow && loginWindow->HasExited)
	{
		delete loginWindow;
		loginWindow = NULL;
	}

	if(localBrowser && localBrowser->HasDone)
	{
		delete localBrowser;
		localBrowser = NULL;
	}
}

void GameController::SetZoomEnabled(bool zoomEnabled)
{
	gameModel->SetZoomEnabled(zoomEnabled);
}

void GameController::SetToolStrength(float value)
{
	gameModel->SetToolStrength(value);
}

void GameController::SetZoomPosition(ui::Point position)
{
	ui::Point zoomPosition = position-(gameModel->GetZoomSize()/2);
	if(zoomPosition.X < 0)
			zoomPosition.X = 0;
	if(zoomPosition.Y < 0)
			zoomPosition.Y = 0;
	if(zoomPosition.X >= XRES-gameModel->GetZoomSize())
			zoomPosition.X = XRES-gameModel->GetZoomSize();
	if(zoomPosition.Y >= YRES-gameModel->GetZoomSize())
			zoomPosition.Y = YRES-gameModel->GetZoomSize();

	ui::Point zoomWindowPosition = ui::Point(0, 0);
	if(position.X < XRES/2)
		zoomWindowPosition.X = XRES-(gameModel->GetZoomSize()*gameModel->GetZoomFactor());

	gameModel->SetZoomPosition(zoomPosition);
	gameModel->SetZoomWindowPosition(zoomWindowPosition);
}

void GameController::SetPaused(bool pauseState)
{
	gameModel->SetPaused(pauseState);
}

void GameController::SetPaused()
{
	gameModel->SetPaused(!gameModel->GetPaused());
}

void GameController::SetDecoration(bool decorationState)
{
	gameModel->SetDecoration(decorationState);
}

void GameController::SetDecoration()
{
	gameModel->SetDecoration(!gameModel->GetDecoration());
}

void GameController::ShowGravityGrid()
{
	gameModel->ShowGravityGrid(!gameModel->GetGravityGrid());
	gameModel->UpdateQuickOptions();
}

void GameController::SetHudEnable(bool hudState)
{
	gameView->SetHudEnable(hudState);
}

bool GameController::GetHudEnable()
{
	return gameView->GetHudEnable();
}

void GameController::SetDebugHUD(bool hudState)
{
	gameView->SetDebugHUD(hudState);
}

bool GameController::GetDebugHUD()
{
	return gameView->GetDebugHUD();
}

void GameController::SetActiveColourPreset(int preset)
{
	gameModel->SetActiveColourPreset(preset);
}

void GameController::SetColour(ui::Colour colour)
{
	gameModel->SetColourSelectorColour(colour);
	gameModel->SetPresetColour(colour);
}

void GameController::SetActiveMenu(int menuID)
{
	gameModel->SetActiveMenu(menuID);
	if(menuID == SC_DECO)
		gameModel->SetColourSelectorVisibility(true);
	else
		gameModel->SetColourSelectorVisibility(false);
}

std::vector<Menu*> GameController::GetMenuList()
{
	return gameModel->GetMenuList();
}

void GameController::ActiveToolChanged(int toolSelection, Tool *tool)
{
	commandInterface->OnActiveToolChanged(toolSelection, tool);
}

Tool * GameController::GetActiveTool(int selection)
{
	return gameModel->GetActiveTool(selection);
}
int var1;
void GameController::SetActiveTool(int toolSelection, Tool * tool)
{

	if (gameModel->GetActiveMenu() == SC_DECO && toolSelection == 2)
		toolSelection = 0;
	gameModel->SetActiveTool(toolSelection, tool);
	gameModel->GetRenderer()->gravityZonesEnabled = false;
	if (toolSelection == 3)
		gameModel->GetSimulation()->replaceModeSelected = tool->GetToolID();
	gameModel->SetLastTool(tool);
	for(int i = 0; i < 3; i++)
	{
		if(gameModel->GetActiveTool(i) == gameModel->GetMenuList().at(SC_WALL)->GetToolList().at(WL_GRAV))
			gameModel->GetRenderer()->gravityZonesEnabled = true;
	}
	if(tool->GetIdentifier() == "DEFAULT_UI_PROPERTY")
		((PropertyTool *)tool)->OpenWindow(gameModel->GetSimulation());

	if(tool->GetIdentifier() == "DEFAULT_TOOL_FPSG")
		gameView->FPSGvar = !gameView->FPSGvar;

	if(tool->GetIdentifier() == "DEFAULT_TOOL_REAL")
		{
		gameView->REALvar = !gameView->REALvar;
		gameModel->GetSimulation()->REALvar = !gameModel->GetSimulation()->REALvar;
		}
	if(tool->GetIdentifier() == "DEFAULT_TOOL_INFO")
		gameView->INFOvar = !gameView->INFOvar;
		
	if(tool->GetIdentifier()== "DEFAULT_TOOL_LINK")
		simulation->LinkVar = !simulation->LinkVar;	
}

Simulation * GameController::GetSimulation()
{
	return gameModel->GetSimulation();
}

int GameController::GetReplaceModeFlags()
{
	return gameModel->GetSimulation()->replaceModeFlags;
}

void GameController::SetReplaceModeFlags(int flags)
{
	gameModel->GetSimulation()->replaceModeFlags = flags;
}

void GameController::OpenSearch(std::string searchText)
{
	if(!search)
		search = new SearchController(new SearchCallback(this));
	if (searchText.length())
		search->DoSearch2(searchText);
	ui::Engine::Ref().ShowWindow(search->GetView());
}

void GameController::OpenLocalSaveWindow(bool asCurrent)
{
	Simulation * sim = gameModel->GetSimulation();
	GameSave * gameSave = sim->Save();
	if(!gameSave)
	{
		new ErrorMessage("Error", "Unable to build save.");
	}
	else
	{
		sim->SaveSimOptions(gameSave);
		gameSave->paused = gameModel->GetPaused();

		std::string filename = "";
		if (gameModel->GetSaveFile())
			filename = gameModel->GetSaveFile()->GetDisplayName();
		SaveFile tempSave(filename);
		tempSave.SetGameSave(gameSave);

		if (!asCurrent || !gameModel->GetSaveFile())
		{
			class LocalSaveCallback: public FileSavedCallback
			{
				GameController * c;
			public:
				LocalSaveCallback(GameController * _c): c(_c) {}
				virtual  ~LocalSaveCallback() {}
				virtual void FileSaved(SaveFile* file)
				{
					c->gameModel->SetSaveFile(file);
				}
			};

			new LocalSaveActivity(tempSave, new LocalSaveCallback(this));
		}
		else if (gameModel->GetSaveFile())
		{
			Client::Ref().MakeDirectory(LOCAL_SAVE_DIR);
			if (Client::Ref().WriteFile(gameSave->Serialise(), gameModel->GetSaveFile()->GetName()))
				new ErrorMessage("Error", "Unable to write save file.");
		}
	}
}

void GameController::LoadSaveFile(SaveFile * file)
{
	gameModel->SetSaveFile(file);
}


void GameController::LoadSave(SaveInfo * save)
{
	gameModel->SetSave(save);
}

void GameController::OpenSavePreview(int saveID, int saveDate, bool instant)
{
	activePreview = new PreviewController(saveID, saveDate, instant, new SaveOpenCallback(this));
	ui::Engine::Ref().ShowWindow(activePreview->GetView());
}

void GameController::OpenSavePreview()
{
	if(gameModel->GetSave())
	{
		activePreview = new PreviewController(gameModel->GetSave()->GetID(), false, new SaveOpenCallback(this));
		ui::Engine::Ref().ShowWindow(activePreview->GetView());
	}
}

void GameController::OpenLocalBrowse()
{
	class LocalSaveOpenCallback: public FileSelectedCallback
	{
		GameController * c;
	public:
		LocalSaveOpenCallback(GameController * _c): c(_c) {}
		virtual  ~LocalSaveOpenCallback() {};
		virtual void FileSelected(SaveFile* file)
		{
			c->LoadSaveFile(file);
			delete file;
		}
	};
	new FileBrowserActivity(LOCAL_SAVE_DIR PATH_SEP, new LocalSaveOpenCallback(this));
}

void GameController::OpenLogin()
{
	loginWindow = new LoginController();
	ui::Engine::Ref().ShowWindow(loginWindow->GetView());
}

void GameController::OpenProfile()
{
	if(Client::Ref().GetAuthUser().ID)
	{
		new ProfileActivity(Client::Ref().GetAuthUser().Username);
	}
	else
	{
		loginWindow = new LoginController();
		ui::Engine::Ref().ShowWindow(loginWindow->GetView());
	}
}

void GameController::OpenElementSearch()
{
	vector<Tool*> toolList;
	vector<Menu*> menuList = gameModel->GetMenuList();
	for(std::vector<Menu*>::iterator iter = menuList.begin(), end = menuList.end(); iter!=end; ++iter) {
		if(!(*iter))
			continue;
		vector<Tool*> menuToolList = (*iter)->GetToolList();
		if(!menuToolList.size())
			continue;
		toolList.insert(toolList.end(), menuToolList.begin(), menuToolList.end());
	}
	vector<Tool*> hiddenTools = gameModel->GetUnlistedTools();
	toolList.insert(toolList.end(), hiddenTools.begin(), hiddenTools.end());
	new ElementSearchActivity(this, toolList);
}

void GameController::OpenColourPicker()
{
	class ColourPickerCallback: public ColourPickedCallback
	{
		GameController * c;
	public:
		ColourPickerCallback(GameController * _c): c(_c) {}
		virtual  ~ColourPickerCallback() {};
		virtual void ColourPicked(ui::Colour colour)
		{
			c->SetColour(colour);
		}
	};
	new ColourPickerActivity(gameModel->GetColourSelectorColour(), new ColourPickerCallback(this));
}

void GameController::OpenTags()
{
	if(gameModel->GetSave() && gameModel->GetSave()->GetID())
	{
		delete tagsWindow;
		tagsWindow = new TagsController(new TagsCallback(this), gameModel->GetSave());
		ui::Engine::Ref().ShowWindow(tagsWindow->GetView());
	}
	else
	{
		new ErrorMessage("Error", "No save open");
	}
}

void GameController::OpenStamps()
{
	localBrowser = new LocalBrowserController(new StampsCallback(this));
	ui::Engine::Ref().ShowWindow(localBrowser->GetView());
}

void GameController::OpenOptions()
{
	options = new OptionsController(gameModel, new OptionsCallback(this));
	ui::Engine::Ref().ShowWindow(options->GetView());

}

void GameController::ShowConsole()
{
	if(!console)
		console = new ConsoleController(NULL, commandInterface);
	if (console->GetView() != ui::Engine::Ref().GetWindow())
		ui::Engine::Ref().ShowWindow(console->GetView());
}

void GameController::HideConsole()
{
	if(!console)
		return;
	if (console->GetView() == ui::Engine::Ref().GetWindow())
		ui::Engine::Ref().CloseWindow();
}

void GameController::OpenRenderOptions()
{
	renderOptions = new RenderController(gameModel->GetRenderer(), NULL);
	ui::Engine::Ref().ShowWindow(renderOptions->GetView());
}

void GameController::OpenSaveWindow()
{
	class SaveUploadedCallback: public ServerSaveActivity::SaveUploadedCallback
	{
		GameController * c;
	public:
		SaveUploadedCallback(GameController * _c): c(_c) {}
		virtual  ~SaveUploadedCallback() {}
		virtual void SaveUploaded(SaveInfo save)
		{
			save.SetVote(1);
			save.SetVotesUp(1);
			c->LoadSave(&save);
		}
	};
	if(gameModel->GetUser().ID)
	{
		Simulation * sim = gameModel->GetSimulation();
		GameSave * gameSave = sim->Save();
		if(!gameSave)
		{
			new ErrorMessage("Error", "Unable to build save.");
		}
		else
		{
			sim->SaveSimOptions(gameSave);
			gameSave->paused = gameModel->GetPaused();

			if(gameModel->GetSave())
			{
				SaveInfo tempSave(*gameModel->GetSave());
				tempSave.SetGameSave(gameSave);
				new ServerSaveActivity(tempSave, new SaveUploadedCallback(this));
			}
			else
			{
				SaveInfo tempSave(0, 0, 0, 0, gameModel->GetUser().Username, "");
				tempSave.SetGameSave(gameSave);
				new ServerSaveActivity(tempSave, new SaveUploadedCallback(this));
			}
		}
	}
	else
	{
		new ErrorMessage("Error", "You need to login to upload saves.");
	}
}

void GameController::SaveAsCurrent()
{

	class SaveUploadedCallback: public ServerSaveActivity::SaveUploadedCallback
	{
		GameController * c;
	public:
		SaveUploadedCallback(GameController * _c): c(_c) {}
		virtual  ~SaveUploadedCallback() {};
		virtual void SaveUploaded(SaveInfo save)
		{
			c->LoadSave(&save);
		}
	};

	if(gameModel->GetSave() && gameModel->GetUser().ID && gameModel->GetUser().Username == gameModel->GetSave()->GetUserName())
	{
		Simulation * sim = gameModel->GetSimulation();
		GameSave * gameSave = sim->Save();
		if(!gameSave)
		{
			new ErrorMessage("Error", "Unable to build save.");
		}
		else
		{
			gameSave->paused = gameModel->GetPaused();
			sim->SaveSimOptions(gameSave);

			if(gameModel->GetSave())
			{
				SaveInfo tempSave(*gameModel->GetSave());
				tempSave.SetGameSave(gameSave);
				new ServerSaveActivity(tempSave, true, new SaveUploadedCallback(this));
			}
			else
			{
				SaveInfo tempSave(0, 0, 0, 0, gameModel->GetUser().Username, "");
				tempSave.SetGameSave(gameSave);
				new ServerSaveActivity(tempSave, true, new SaveUploadedCallback(this));
			}
		}
	}
	else if(gameModel->GetUser().ID)
	{
		OpenSaveWindow();
	}
	else
	{
		new ErrorMessage("Error", "You need to login to upload saves.");
	}
}

void GameController::FrameStep()
{
	gameModel->FrameStep(1);
	gameModel->SetPaused(true);
}

void GameController::Vote(int direction)
{
	if(gameModel->GetSave() && gameModel->GetUser().ID && gameModel->GetSave()->GetID() && gameModel->GetSave()->GetVote()==0)
	{
		try
		{
			gameModel->SetVote(direction);
		}
		catch(GameModelException & ex)
		{
			new ErrorMessage("Error while voting", ex.what());
		}
	}
}

void GameController::ChangeBrush()
{
	gameModel->SetBrushID(gameModel->GetBrushID()+1);
	BrushChanged(gameModel->GetBrushID(), gameModel->GetBrush()->GetRadius().X, gameModel->GetBrush()->GetRadius().Y);
}

void GameController::ClearSim()
{
	gameModel->SetSave(NULL);
	gameModel->ClearSimulation();
}

void GameController::ReloadSim()
{
	if(gameModel->GetSave() && gameModel->GetSave()->GetGameSave())
	{
		gameModel->SetSave(gameModel->GetSave());
	}
	else if(gameModel->GetSaveFile() && gameModel->GetSaveFile()->GetGameSave())
	{
		gameModel->SetSaveFile(gameModel->GetSaveFile());
	}
}

#ifdef PARTICLEDEBUG
void GameController::ParticleDebug(int mode, int x, int y)
{
	Simulation *sim = gameModel->GetSimulation();
	int debug_currentParticle = sim->debug_currentParticle;
	int i;
	std::stringstream logmessage;

	if (mode == 0)
	{
		if (!sim->NUM_PARTS)
			return;
		i = debug_currentParticle;
		while (i < NPART && !sim->parts[i].type)
			i++;
		if (i == NPART)
			logmessage << "End of particles reached, updated sim";
		else
			logmessage << "Updated particle #" << i;
	}
	else if (mode == 1)
	{
		if (x < 0 || x >= XRES || y < 0 || y >= YRES || !(i = (sim->pmap[y][x]>>8)) || i < debug_currentParticle)
		{
			i = NPART;
			logmessage << "Updated particles from #" << debug_currentParticle << " to end, updated sim";
		}
		else
			logmessage << "Updated particles #" << debug_currentParticle << " through #" << i;
	}
	gameModel->Log(logmessage.str());

	sim->UpdateParticles(debug_currentParticle, i);
	if (i < NPART-1)
		sim->debug_currentParticle = i+1;
	else
	{
		sim->framerender = 1;
		sim->UpdateSim();
		sim->framerender = 0;
		sim->debug_currentParticle = 0;
	}
}
#endif

std::string GameController::ElementResolve(int type, int ctype)
{
	if(gameModel && gameModel->GetSimulation())
	{
		if (type == PT_LIFE && ctype >= 0 && ctype < NGOL && gameModel->GetSimulation()->gmenu)
			return gameModel->GetSimulation()->gmenu[ctype].name;
		else if (type >= 0 && type < PT_NUM && gameModel->GetSimulation()->elements)
			return std::string(gameModel->GetSimulation()->elements[type].Name);
	}
	return "";
}

bool GameController::IsValidElement(int type)
{
	if(gameModel && gameModel->GetSimulation())
	{
		return (type && gameModel->GetSimulation()->IsValidElement(type));
	}
	else
		return false;
}

std::string GameController::WallName(int type)
{
	if(gameModel && gameModel->GetSimulation() && gameModel->GetSimulation()->wtypes && type >= 0 && type < UI_WALLCOUNT)
		return std::string(gameModel->GetSimulation()->wtypes[type].name);
	else
		return "";
}

void GameController::NotifyAuthUserChanged(Client * sender)
{
	User newUser = sender->GetAuthUser();
	gameModel->SetUser(newUser);
}

void GameController::NotifyNewNotification(Client * sender, std::pair<std::string, std::string> notification)
{
	class LinkNotification : public Notification
	{
		std::string link;
	public:
		LinkNotification(std::string link_, std::string message) : Notification(message), link(link_) {}
		virtual ~LinkNotification() {}

		virtual void Action()
		{
			OpenURI(link);
		}
	};
	gameModel->AddNotification(new LinkNotification(notification.second, notification.first));
}

void GameController::NotifyUpdateAvailable(Client * sender)
{
	class UpdateConfirmation: public ConfirmDialogueCallback {
	public:
		GameController * c;
		UpdateConfirmation(GameController * c_) {	c = c_;	}
		virtual void ConfirmCallback(ConfirmPrompt::DialogueResult result) {
			if (result == ConfirmPrompt::ResultOkay)
			{
				c->RunUpdater();
			}
		}
		virtual ~UpdateConfirmation() { }
	};

	class UpdateNotification : public Notification
	{
		GameController * c;
	public:
		UpdateNotification(GameController * c, std::string message) : Notification(message), c(c) {}
		virtual ~UpdateNotification() {}

		virtual void Action()
		{
			std::string currentVersion, newVersion;
#ifdef BETA
			currentVersion = MTOS(SAVE_VERSION) "." MTOS(MINOR_VERSION) " Beta, Build " MTOS(BUILD_NUM);
#elif defined(SNAPSHOT)
			currentVersion = "Snapshot " MTOS(SNAPSHOT_ID);
#else
			currentVersion = MTOS(SAVE_VERSION) "." MTOS(MINOR_VERSION) " Stable, Build " MTOS(BUILD_NUM);
#endif

			UpdateInfo info = Client::Ref().GetUpdateInfo();
			if(info.Type == UpdateInfo::Beta)
				newVersion = format::NumberToString<int>(info.Major) + " " + format::NumberToString<int>(info.Minor) + " Beta, Build " + format::NumberToString<int>(info.Build);
			else if(info.Type == UpdateInfo::Snapshot)
				newVersion = "Snapshot " + format::NumberToString<int>(info.Time);
			else if(info.Type == UpdateInfo::Stable)
				newVersion = format::NumberToString<int>(info.Major) + " " + format::NumberToString<int>(info.Minor) + " Stable, Build " + format::NumberToString<int>(info.Build);

			new ConfirmPrompt("Run Updater", "Are you sure you want to run the updater, please save any changes before updating.\n\nCurrent version:\n " + currentVersion + "\nNew version:\n " + newVersion, new UpdateConfirmation(c));
		}
	};

	switch(sender->GetUpdateInfo().Type)
	{
		case UpdateInfo::Snapshot:
			gameModel->AddNotification(new UpdateNotification(this, std::string("A new snapshot is available - click here to update")));
			break;
		case UpdateInfo::Stable:
			gameModel->AddNotification(new UpdateNotification(this, std::string("A new version is available - click here to update")));
			break;
		case UpdateInfo::Beta:
			gameModel->AddNotification(new UpdateNotification(this, std::string("A new beta is available - click here to update")));
			break;
	}
}

void GameController::RemoveNotification(Notification * notification)
{
	gameModel->RemoveNotification(notification);
}

void GameController::RunUpdater()
{
	Exit();
	new UpdateActivity();
}
