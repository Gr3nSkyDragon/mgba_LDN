/* Copyright (c) 2013-2018 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ActionMapper.h"
#include "moc_ActionMapper.cpp"

#include "ConfigController.h"
#include "ShortcutController.h"

#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>

using namespace QGBA;

static const char* const kKeepMenuOpen = "mgbaKeepMenuOpen";
static const char* const kClearConnected = "mgbaClearConnected";

void ActionMapper::addMenu(const QString& visibleName, const QString& name, const QString& parent) {
	QString mname(QString(".%1").arg(name));
	m_menus[parent].append(mname);
	m_reverseMenus[mname] = parent;
	m_menuNames[name] = visibleName;
}

void ActionMapper::addHiddenMenu(const QString& visibleName, const QString& name, const QString& parent) {
	m_hiddenActions.insert(QString(".%1").arg(name));
	addMenu(visibleName, name, parent);
}

void ActionMapper::clearMenu(const QString& name) {
	m_menus[name].clear();
	emit menuCleared(name);
}

void ActionMapper::setMenuVisible(const QString& name, bool visible) {
	if (visible) {
		m_invisibleMenus.remove(name);
	} else {
		m_invisibleMenus.insert(name);
	}
	QPointer<QMenu> qmenu = m_qmenus.value(name);
	if (qmenu) {
		qmenu->menuAction()->setVisible(visible);
	}
}

void ActionMapper::rebuildMenu(QMenuBar* menubar, QWidget* context, const ShortcutController& shortcuts) {
	menubar->clear();
	m_menu.clear();
	for (QAction* action : context->actions()) {
		context->removeAction(action);
	}
	for (const QString& m : m_menus[{}]) {
		if (m_hiddenActions.contains(m)) {
			continue;
		}
		QString menu = m.mid(1);
		QMenu* qmenu = menubar->addMenu(m_menuNames[menu]);

		rebuildMenu(menu, qmenu, context, shortcuts);
		m_menu.addMenu(qmenu);
	}
}

bool ActionMapper::rebuildSubmenu(const QString& menu, QWidget* context, const ShortcutController& shortcuts) {
	QPointer<QMenu> qmenu = m_qmenus.value(menu);
	if (!qmenu) {
		return false;
	}
	for (QAction* action : qmenu->actions()) {
		qmenu->removeAction(action);
	}
	rebuildMenu(menu, qmenu, context, shortcuts);
	return true;
}

void ActionMapper::rebuildMenu(const QString& menu, QMenu* qmenu, QWidget* context, const ShortcutController& shortcuts) {
	m_qmenus[menu] = qmenu;
	qmenu->installEventFilter(this);
	for (const QString& actionName : m_menus[menu]) {
		if (actionName.isNull()) {
			qmenu->addSeparator();
			continue;
		}
		if (m_hiddenActions.contains(actionName)) {
			continue;
		}
		if (actionName[0] == '.') {
			QString name = actionName.mid(1);
			QMenu* newMenu = qmenu->addMenu(m_menuNames[name]);
			newMenu->menuAction()->setVisible(!m_invisibleMenus.contains(name));
			rebuildMenu(name, newMenu, context, shortcuts);
			continue;
		}
		auto action = getAction(actionName);
		QAction* qaction = qmenu->addAction(action->visibleName());
		qaction->setEnabled(action->isEnabled());
		qaction->setShortcutContext(Qt::WidgetShortcut);
		if (action->isExclusive() || action->booleanAction()) {
			qaction->setCheckable(true);
		}
		if (action->isActive()) {
			qaction->setChecked(true);
		}
		qaction->setProperty(kKeepMenuOpen, action->keepsMenuOpen());
		const Shortcut* shortcut = shortcuts.shortcut(actionName);
		if (shortcut) {
			if (shortcut->shortcut() > 0) {
				qaction->setShortcut(QKeySequence(shortcut->shortcut()));
			}
		} else if (!m_defaultShortcuts[actionName].isEmpty()) {
			qaction->setShortcut(m_defaultShortcuts[actionName][0]);
		}
		switch (action->role()) {
		case Action::Role::NO_ROLE:
			qaction->setMenuRole(QAction::NoRole);
			break;
		case Action::Role::SETTINGS:
			qaction->setMenuRole(QAction::PreferencesRole);
			break;
		case Action::Role::ABOUT:
			qaction->setMenuRole(QAction::AboutRole);
			break;
		case Action::Role::QUIT:
			qaction->setMenuRole(QAction::QuitRole);
			break;
		}

		std::weak_ptr<Action> weakAction(action);
		QObject::connect(qaction, &QAction::triggered, [qaction, weakAction](bool enabled) {
			if (weakAction.expired()) {
				return;
			}
			std::shared_ptr<Action> action(weakAction.lock());
			if (qaction->isCheckable()) {
				action->trigger(enabled);
			} else {
				action->trigger();
			}
		});
		QObject::connect(action.get(), &Action::enabled, qaction, &QAction::setEnabled);
		QObject::connect(action.get(), &Action::visibleNameChanged, qaction, &QAction::setText);
		QObject::connect(action.get(), &Action::activated, [qaction, weakAction = std::move(weakAction)](bool active) {
			std::shared_ptr<Action> action(weakAction.lock());
			if (qaction->isCheckable()) {
				qaction->setChecked(active);
			} else if (active) {
				action->setActive(false);
			}
		});
		QObject::connect(action.get(), &Action::destroyed, qaction, &QAction::deleteLater);
		if (shortcut) {
			QObject::connect(shortcut, &Shortcut::shortcutChanged, qaction, [qaction](int shortcut) {
				qaction->setShortcut(QKeySequence(shortcut));
			});
		}
		context->addAction(qaction);
	}
	if (qmenu->property(kClearConnected).toBool()) {
		return; // refilled in place by rebuildSubmenu: already connected
	}
	qmenu->setProperty(kClearConnected, true);
	connect(this, &ActionMapper::menuCleared, qmenu, [qmenu, menu](const QString& name) {
		if (name != menu) {
			return;
		}
		for (QAction* action : qmenu->actions()) {
			qmenu->removeAction(action);
		}
	});
}

// A QMenu closes itself when one of its items is triggered; for an action that keeps the menu open, trigger it here
// and swallow the event instead.
bool ActionMapper::eventFilter(QObject* obj, QEvent* event) {
	QMenu* qmenu = qobject_cast<QMenu*>(obj);
	if (!qmenu) {
		return QObject::eventFilter(obj, event);
	}
	QAction* qaction = nullptr;
	if (event->type() == QEvent::MouseButtonRelease) {
		QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
		if (mouse->button() == Qt::LeftButton) {
			qaction = qmenu->actionAt(mouse->pos());
		}
	} else if (event->type() == QEvent::KeyPress) {
		int key = static_cast<QKeyEvent*>(event)->key();
		if (key == Qt::Key_Return || key == Qt::Key_Enter) {
			qaction = qmenu->activeAction();
		}
	}
	if (qaction && qaction->isEnabled() && !qaction->menu() && qaction->property(kKeepMenuOpen).toBool()) {
		qaction->trigger();
		return true;
	}
	return QObject::eventFilter(obj, event);
}

void ActionMapper::addSeparator(const QString& menu) {
	m_menus[menu].append(QString{});
}

std::shared_ptr<Action> ActionMapper::addAction(const Action& act, const QString& name, const QString& menu, const QKeySequence& shortcut) {
	m_actions.insert(name, std::make_shared<Action>(act));
	m_reverseMenus[name] = menu;
	m_menus[menu].append(name);
	if (!shortcut.isEmpty()) {
		m_defaultShortcuts[name] = shortcut;
	}
	emit actionAdded(name);

	return getAction(name);
}

std::shared_ptr<Action> ActionMapper::addAction(const QString& visibleName, const QString& name, Action::Function&& action, const QString& menu, const QKeySequence& shortcut) {
	return addAction(Action(std::move(action), name, visibleName), name, menu, shortcut);
}

std::shared_ptr<Action> ActionMapper::addAction(const QString& visibleName, ConfigOption* option, const QVariant& variant, const QString& menu) {
	return addAction(Action([option, variant]() {
		option->setValue(variant);
	}, option->name(), visibleName), QString("%1.%2").arg(option->name()).arg(variant.toString()), menu, {});
}

std::shared_ptr<Action> ActionMapper::addBooleanAction(const QString& visibleName, const QString& name, Action::BooleanFunction&& action, const QString& menu, const QKeySequence& shortcut) {
	return addAction(Action(std::move(action), name, visibleName), name, menu, shortcut);
}

std::shared_ptr<Action> ActionMapper::addBooleanAction(const QString& visibleName, ConfigOption* option, const QString& menu) {
	return addAction(Action([option](bool value) {
		option->setValue(value);
	}, option->name(), visibleName), option->name(), menu, {});
}

std::shared_ptr<Action> ActionMapper::addHeldAction(const QString& visibleName, const QString& name, Action::BooleanFunction&& action, const QString& menu, const QKeySequence& shortcut) {
	m_hiddenActions.insert(name);
	m_heldActions.insert(name);
	return addBooleanAction(visibleName, name, std::move(action), menu, shortcut);
}

std::shared_ptr<Action> ActionMapper::addHiddenAction(const QString& visibleName, const QString& name, Action::Function&& action, const QString& menu, const QKeySequence& shortcut) {
	m_hiddenActions.insert(name);
	return addAction(visibleName, name, std::move(action), menu, shortcut);
}

QStringList ActionMapper::menuItems(const QString& menu) const {
	return m_menus[menu];
}

QString ActionMapper::menuFor(const QString& menu) const {
	return m_reverseMenus[menu];
}

QString ActionMapper::menuName(const QString& menu) const {
	if (!menu.isNull() && menu[0] == '.') {
		return m_menuNames[menu.mid(1)];
	}
	return m_menuNames[menu];
}

std::shared_ptr<Action> ActionMapper::getAction(const QString& itemName) {
	return m_actions.value(itemName);
}

QKeySequence ActionMapper::defaultShortcut(const QString& itemName) {
	return m_defaultShortcuts[itemName];
}

void ActionMapper::exec(const QPoint& pos) {
	m_menu.exec(pos);
}
