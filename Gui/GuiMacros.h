//
//  GuiMacros.h
//  Gui
//
//  Created by Frédéric Devernay on 19/08/2014.
//
//

#ifndef Gui_GuiMacros_h
#define Gui_GuiMacros_h

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

// the following macro only tests the Control, Alt, and Shift modifiers, and discards the others
#define modCAS(e) ( (e)->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier) )

#define modCASIsNone(e)            ( modCAS(e) == (Qt::NoModifier) )
#define modCASIsControl(e)         ( modCAS(e) == (Qt::ControlModifier) )
#define modCASIsControlShift(e)    ( modCAS(e) == (Qt::ControlModifier | Qt::ShiftModifier) )
#define modCASIsControlAlt(e)      ( modCAS(e) == (Qt::ControlModifier | Qt::AltModifier) )
#define modCASIsControlAltShift(e) ( modCAS(e) == (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier) )
#define modCASIsAlt(e)             ( modCAS(e) == (Qt::AltModifier) )
#define modCASIsAltShift(e)        ( modCAS(e) == (Qt::AltModifier | Qt::ShiftModifier) )
#define modCASIsShift(e)           ( modCAS(e) == (Qt::ShiftModifier) )

// macros to test if the modifier is present (but there may be other modifiers too)
#define modifierHasControl(e)         ( (e)->modifiers().testFlag(Qt::ControlModifier) )
#define modifierHasAlt(e)             ( (e)->modifiers().testFlag(Qt::AltModifier) )
#define modifierHasShift(e)           ( (e)->modifiers().testFlag(Qt::ShiftModifier) )

// macros to test if a button is held down (pressed), or a single-button compatibility combination
//Right click emulated with Left + MetaModifier, which is the way its done everywhere else on the mac
#define buttonControlAlt(e)          ( (e)->modifiers() & (Qt::MetaModifier | Qt::AltModifier) )

/// THE GOOD VERSION
// PLEASE DON'T BREAK THIS.
// THANK YOU VERY VERY VERY MUCH.
#define buttonDownIsLeft(e)              ( ( (e)->buttons() == Qt::LeftButton   && buttonControlAlt(e) == Qt::NoModifier ) )

//Right click emulated with Left + MetaModifier, which is the way its done everywhere else on the mac
#define buttonDownIsRight(e)             ( ( (e)->buttons() == Qt::RightButton  && buttonControlAlt(e) == Qt::NoModifier ) || \
                                           ( (e)->buttons() == Qt::LeftButton   && buttonControlAlt(e) == Qt::MetaModifier ) )

#define triggerButtonisLeft(e) ( (e)->button() == Qt::LeftButton && buttonControlAlt(e) == Qt::NoModifier )
#define triggerButtonisRight(e) ( (e)->button() == Qt::RightButton || \
                                  ( (e)->button() == Qt::LeftButton   && buttonControlAlt(e) == Qt::ControlModifier ) )


#define buttonDownIsMiddle(e)            ( ( (e)->buttons() == Qt::MiddleButton && buttonControlAlt(e) == Qt::NoModifier ) || \
                                           ( (e)->buttons() == Qt::LeftButton   && buttonControlAlt(e) == Qt::AltModifier ) )


// macros to test the button that triggered the event
#define triggerButtonisMiddle(e) ( (e)->button() == Qt::MiddleButton || \
                                   ( (e)->button() == Qt::LeftButton   && buttonControlAlt(e) == Qt::AltModifier ) )


// macros to test
#define buttonModifier(e)            ( (e)->modifiers() & ( Qt::KeyboardModifierMask & ~(Qt::MetaModifier | Qt::AltModifier) ) )
#define buttonModifierIsNone(e)      (buttonModifier(e) == Qt::NoModifier)
#define buttonModifierIsShift(e)     (buttonModifier(e) == Qt::ShiftModifier)
#define buttonModifierIsControl(e)   (buttonModifier(e) == Qt::ControlModifier)

#endif // ifndef Gui_GuiMacros_h
