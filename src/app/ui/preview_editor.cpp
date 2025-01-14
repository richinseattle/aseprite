// Aseprite
// Copyright (C) 2001-2015  David Capello
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/preview_editor.h"

#include "app/app.h"
#include "app/document.h"
#include "app/ini_file.h"
#include "app/loop_tag.h"
#include "app/modules/editors.h"
#include "app/modules/gui.h"
#include "app/pref/preferences.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_view.h"
#include "app/ui/editor/navigate_state.h"
#include "app/ui/editor/play_state.h"
#include "app/ui/skin/skin_button.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "app/ui/toolbar.h"
#include "app/ui_context.h"
#include "base/bind.h"
#include "doc/sprite.h"
#include "gfx/rect.h"
#include "ui/base.h"
#include "ui/button.h"
#include "ui/close_event.h"
#include "ui/message.h"
#include "ui/system.h"

#include "doc/frame_tag.h"

namespace app {

using namespace app::skin;
using namespace ui;

class MiniCenterButton : public SkinButton<CheckBox> {
public:
  MiniCenterButton()
    : SkinButton<CheckBox>(
      SkinTheme::instance()->parts.windowCenterButtonNormal(),
      SkinTheme::instance()->parts.windowCenterButtonHot(),
      SkinTheme::instance()->parts.windowCenterButtonSelected())
  {
    setup_bevels(this, 0, 0, 0, 0);
    setDecorative(true);
    setSelected(true);
  }

protected:
  void onSetDecorativeWidgetBounds() override {
    SkinTheme* theme = static_cast<SkinTheme*>(this->theme());
    Widget* window = parent();
    gfx::Rect rect(0, 0, 0, 0);
    gfx::Size iconSize = theme->parts.windowPlayButtonNormal()->size();
    gfx::Size closeSize = theme->parts.windowCloseButtonNormal()->size();

    rect.w = iconSize.w;
    rect.h = iconSize.h;

    rect.offset(window->bounds().x2() - 3*guiscale()
      - iconSize.w - 1*guiscale()
      - iconSize.w - 1*guiscale() - closeSize.w,
      window->bounds().y + 3*guiscale());

    setBounds(rect);
  }

  bool onProcessMessage(Message* msg) override
  {
    switch (msg->type()) {

      case kSetCursorMessage:
        ui::set_mouse_cursor(kArrowCursor);
        return true;
    }

    return SkinButton<CheckBox>::onProcessMessage(msg);
  }
};

class MiniPlayButton : public SkinButton<Button> {
public:
  MiniPlayButton()
    : SkinButton<Button>(SkinPartPtr(nullptr),
                         SkinPartPtr(nullptr),
                         SkinPartPtr(nullptr))
    , m_isPlaying(false) {
    setupIcons();
    setup_bevels(this, 0, 0, 0, 0);
    setDecorative(true);
  }

  bool isPlaying() const { return m_isPlaying; }

  void stop() {
    m_isPlaying = false;
    setupIcons();
  }

  base::Signal0<void> Popup;

private:

  void onClick(Event& ev) override {
    m_isPlaying = !m_isPlaying;
    setupIcons();

    SkinButton<Button>::onClick(ev);
  }

  void onSetDecorativeWidgetBounds() override {
    SkinTheme* theme = static_cast<SkinTheme*>(this->theme());
    Widget* window = parent();
    gfx::Rect rect(0, 0, 0, 0);
    gfx::Size playSize = theme->parts.windowPlayButtonNormal()->size();
    gfx::Size closeSize = theme->parts.windowCloseButtonNormal()->size();

    rect.w = playSize.w;
    rect.h = playSize.h;

    rect.offset(window->bounds().x2() - 3*guiscale()
      - playSize.w - 1*guiscale() - closeSize.w,
      window->bounds().y + 3*guiscale());

    setBounds(rect);
  }

  bool onProcessMessage(Message* msg) override {
    switch (msg->type()) {

      case kSetCursorMessage:
        ui::set_mouse_cursor(kArrowCursor);
        return true;

      case kMouseUpMessage: {
        MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);
        if (mouseMsg->right()) {
          if (hasCapture()) {
            releaseMouse();
            Popup();

            setSelected(false);
            return true;
          }
        }
        break;
      }
    }

    return SkinButton<Button>::onProcessMessage(msg);
  }

  void setupIcons() {
    SkinTheme* theme = SkinTheme::instance();

    if (m_isPlaying)
      setParts(theme->parts.windowStopButtonNormal(),
               theme->parts.windowStopButtonHot(),
               theme->parts.windowStopButtonSelected());
    else
      setParts(theme->parts.windowPlayButtonNormal(),
               theme->parts.windowPlayButtonHot(),
               theme->parts.windowPlayButtonSelected());
  }

  bool m_isPlaying;
};

PreviewEditorWindow::PreviewEditorWindow()
  : Window(WithTitleBar, "Preview")
  , m_docView(NULL)
  , m_centerButton(new MiniCenterButton())
  , m_playButton(new MiniPlayButton())
  , m_refFrame(0)
  , m_aniSpeed(1.0)
  , m_relatedEditor(nullptr)
{
  setChildSpacing(0);
  setAutoRemap(false);
  setWantFocus(false);

  m_isEnabled = get_config_bool("MiniEditor", "Enabled", true);

  m_centerButton->Click.connect(base::Bind<void>(&PreviewEditorWindow::onCenterClicked, this));
  m_playButton->Click.connect(base::Bind<void>(&PreviewEditorWindow::onPlayClicked, this));
  m_playButton->Popup.connect(base::Bind<void>(&PreviewEditorWindow::onPopupSpeed, this));

  addChild(m_centerButton);
  addChild(m_playButton);
}

PreviewEditorWindow::~PreviewEditorWindow()
{
  set_config_bool("MiniEditor", "Enabled", m_isEnabled);
}

void PreviewEditorWindow::setPreviewEnabled(bool state)
{
  m_isEnabled = state;

  updateUsingEditor(current_editor);
}

bool PreviewEditorWindow::onProcessMessage(ui::Message* msg)
{
  switch (msg->type()) {

    case kOpenMessage:
      {
        SkinTheme* theme = SkinTheme::instance();

        // Default bounds
        int width = ui::display_w()/4;
        int height = ui::display_h()/4;
        int extra = 2*theme->dimensions.miniScrollbarSize();
        setBounds(
          gfx::Rect(
            ui::display_w() - width - ToolBar::instance()->bounds().w - extra,
            ui::display_h() - height - StatusBar::instance()->bounds().h - extra,
            width, height));

        load_window_pos(this, "MiniEditor");
        invalidate();
      }
      break;

    case kCloseMessage:
      save_window_pos(this, "MiniEditor");
      break;

  }

  return Window::onProcessMessage(msg);
}

void PreviewEditorWindow::onClose(ui::CloseEvent& ev)
{
  Button* closeButton = dynamic_cast<Button*>(ev.getSource());
  if (closeButton != NULL &&
      closeButton->id() == SkinTheme::kThemeCloseButtonId) {
    // Here we don't use "setMiniEditorEnabled" to change the state of
    // "m_isEnabled" because we're coming from a close event of the
    // window.
    m_isEnabled = false;

    // Redraw the tool bar because it shows the mini editor enabled
    // state. TODO abstract this event
    ToolBar::instance()->invalidate();

    destroyDocView();
  }
}

void PreviewEditorWindow::onWindowResize()
{
  Window::onWindowResize();

  DocumentView* view = UIContext::instance()->activeView();
  if (view)
    updateUsingEditor(view->getEditor());
}

void PreviewEditorWindow::onCenterClicked()
{
  if (m_centerButton->isSelected()) {
    DocumentView* view = UIContext::instance()->activeView();
    if (view)
      updateUsingEditor(view->getEditor());
  }
}

void PreviewEditorWindow::onPlayClicked()
{
  Editor* miniEditor = (m_docView ? m_docView->getEditor(): nullptr);
  if (!miniEditor || !miniEditor->document())
    return;

  if (m_playButton->isPlaying()) {
    m_refFrame = miniEditor->frame();
    miniEditor->play(Preferences::instance().preview.playOnce());
  }
  else
    miniEditor->stop();
}

void PreviewEditorWindow::onPopupSpeed()
{
  Editor* miniEditor = (m_docView ? m_docView->getEditor(): nullptr);
  if (!miniEditor || !miniEditor->document())
    return;

  miniEditor->showAnimationSpeedMultiplierPopup(
    Preferences::instance().preview.playOnce, false);
  m_aniSpeed = miniEditor->getAnimationSpeedMultiplier();
}

void PreviewEditorWindow::updateUsingEditor(Editor* editor)
{
  if (!m_isEnabled || !editor) {
    hideWindow();
    m_relatedEditor = nullptr;
    return;
  }

  if (!editor->isActive())
    return;

  m_relatedEditor = editor;

  Document* document = editor->document();
  Editor* miniEditor = (m_docView ? m_docView->getEditor(): NULL);

  if (!isVisible())
    openWindow();

  gfx::Rect visibleBounds = editor->getVisibleSpriteBounds();
  gfx::Point centerPoint = visibleBounds.center();
  bool center = (m_centerButton->isSelected());

  // Set the same location as in the given editor.
  if (!miniEditor || miniEditor->document() != document) {
    destroyDocView();

    m_docView = new DocumentView(document, DocumentView::Preview);
    addChild(m_docView);

    miniEditor = m_docView->getEditor();
    miniEditor->setZoom(render::Zoom(1, 1));
    miniEditor->setLayer(editor->layer());
    miniEditor->setFrame(editor->frame());
    miniEditor->setState(EditorStatePtr(new NavigateState));
    miniEditor->setAnimationSpeedMultiplier(m_aniSpeed);
    miniEditor->addObserver(this);
    layout();
    center = true;
  }

  if (center)
    miniEditor->centerInSpritePoint(centerPoint);

  if (!m_playButton->isPlaying()) {
    miniEditor->stop();
    miniEditor->setLayer(editor->layer());
    miniEditor->setFrame(editor->frame());
  }
  else {
    if (miniEditor->isPlaying()) {
      doc::FrameTag* tag = get_animation_tag(editor->sprite(), editor->frame());
      doc::FrameTag* playingTag = get_animation_tag(editor->sprite(), m_refFrame);
      if (tag != playingTag)
        miniEditor->stop();
    }

    if (!miniEditor->isPlaying())
      miniEditor->setFrame(m_refFrame = editor->frame());

    miniEditor->play(Preferences::instance().preview.playOnce());
  }
}

void PreviewEditorWindow::uncheckCenterButton()
{
  if (m_centerButton->isSelected())
    m_centerButton->setSelected(false);
}

void PreviewEditorWindow::onStateChanged(Editor* editor)
{
  EditorStatePtr state = editor->getState();
  PlayState* playState = (state ? dynamic_cast<PlayState*>(state.get()): nullptr);
  if (!playState) {
    // We have to switch the "play button" state to "play" because the
    // editor animation has just stopped. This happens when we use
    // "play once" option and the PlayState stops automatically.
    m_playButton->stop();
  }
}

void PreviewEditorWindow::hideWindow()
{
  destroyDocView();
  if (isVisible())
    closeWindow(NULL);
}

void PreviewEditorWindow::destroyDocView()
{
  if (m_docView) {
    m_docView->getEditor()->removeObserver(this);

    delete m_docView;
    m_docView = nullptr;
  }
}

} // namespace app
