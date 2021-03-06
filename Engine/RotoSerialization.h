//  Natron
//
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 * Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 * contact: immarespond at gmail dot com
 *
 */
#ifndef ROTOSERIALIZATION_H
#define ROTOSERIALIZATION_H

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "Engine/RotoContext.h"
#include "Engine/RotoContextPrivate.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/map.hpp>
#endif

#include "Engine/AppManager.h"
#include "Engine/CurveSerialization.h"
#include "Engine/KnobSerialization.h"

#define ROTO_DRAWABLE_ITEM_INTRODUCES_COMPOSITING 2
#define ROTO_DRAWABLE_ITEM_REMOVES_INVERTED 3
#define ROTO_DRAWABLE_ITEM_VERSION ROTO_DRAWABLE_ITEM_REMOVES_INVERTED

#define BEZIER_CP_INTRODUCES_OFFSET 2
#define BEZIER_CP_FIX_BUG_CURVE_POINTER 3
#define BEZIER_CP_VERSION BEZIER_CP_FIX_BUG_CURVE_POINTER

#define ROTO_ITEM_INTRODUCES_LABEL 2
#define ROTO_ITEM_VERSION ROTO_ITEM_INTRODUCES_LABEL

#define ROTO_CTX_REMOVE_COUNTERS 2
#define ROTO_CTX_VERSION ROTO_CTX_REMOVE_COUNTERS

#define ROTO_LAYER_SERIALIZATION_REMOVES_IS_BEZIER 2
#define ROTO_LAYER_SERIALIZATION_VERSION ROTO_LAYER_SERIALIZATION_REMOVES_IS_BEZIER

#define BEZIER_SERIALIZATION_INTRODUCES_ROTO_STROKE 2
#define BEZIER_SERIALIZATION_VERSION BEZIER_SERIALIZATION_INTRODUCES_ROTO_STROKE

template<class Archive>
void
BezierCP::save(Archive & ar,
                    const unsigned int version) const
{
    (void)version;
    ar & boost::serialization::make_nvp("X",_imp->x);
    ar & boost::serialization::make_nvp("X_animation",*_imp->curveX);
    ar & boost::serialization::make_nvp("Y",_imp->y);
    ar & boost::serialization::make_nvp("Y_animation",*_imp->curveY);
    ar & boost::serialization::make_nvp("Left_X",_imp->leftX);
    ar & boost::serialization::make_nvp("Left_X_animation",*_imp->curveLeftBezierX);
    ar & boost::serialization::make_nvp("Left_Y",_imp->leftY);
    ar & boost::serialization::make_nvp("Left_Y_animation",*_imp->curveLeftBezierY);
    ar & boost::serialization::make_nvp("Right_X",_imp->rightX);
    ar & boost::serialization::make_nvp("Right_X_animation",*_imp->curveRightBezierX);
    ar & boost::serialization::make_nvp("Right_Y",_imp->rightY);
    ar & boost::serialization::make_nvp("Right_Y_animation",*_imp->curveRightBezierY);
    if (version >= BEZIER_CP_INTRODUCES_OFFSET) {
        QWriteLocker l(&_imp->masterMutex);
        ar & boost::serialization::make_nvp("OffsetTime",_imp->offsetTime);
    }
}

template<class Archive>
void
BezierCP::load(Archive & ar,
               const unsigned int version) 
{
    bool createdDuringToRC2Or3 = appPTR->wasProjectCreatedDuringRC2Or3();
    if (version >= BEZIER_CP_FIX_BUG_CURVE_POINTER || !createdDuringToRC2Or3) {
    
        ar & boost::serialization::make_nvp("X",_imp->x);
        
        Curve xCurve;
        ar & boost::serialization::make_nvp("X_animation",xCurve);
        _imp->curveX->clone(xCurve);
        
        if (version < BEZIER_CP_FIX_BUG_CURVE_POINTER) {
            Curve curveBug;
            ar & boost::serialization::make_nvp("Y",curveBug);
        } else {
            ar & boost::serialization::make_nvp("Y",_imp->y);
        }
        
        Curve yCurve;
        ar & boost::serialization::make_nvp("Y_animation",yCurve);
        _imp->curveY->clone(yCurve);
        
        ar & boost::serialization::make_nvp("Left_X",_imp->leftX);
        
        Curve leftCurveX,leftCurveY,rightCurveX,rightCurveY;
        
        ar & boost::serialization::make_nvp("Left_X_animation",leftCurveX);
        ar & boost::serialization::make_nvp("Left_Y",_imp->leftY);
        ar & boost::serialization::make_nvp("Left_Y_animation",leftCurveY);
        ar & boost::serialization::make_nvp("Right_X",_imp->rightX);
        ar & boost::serialization::make_nvp("Right_X_animation",rightCurveX);
        ar & boost::serialization::make_nvp("Right_Y",_imp->rightY);
        ar & boost::serialization::make_nvp("Right_Y_animation",rightCurveY);
        
        _imp->curveLeftBezierX->clone(leftCurveX);
        _imp->curveLeftBezierY->clone(leftCurveY);
        _imp->curveRightBezierX->clone(rightCurveX);
        _imp->curveRightBezierY->clone(rightCurveY);
        
    } else {
        ar & boost::serialization::make_nvp("X",_imp->x);
        
        boost::shared_ptr<Curve> xCurve,yCurve,leftCurveX,leftCurveY,rightCurveX,rightCurveY;
        ar & boost::serialization::make_nvp("X_animation",xCurve);
        _imp->curveX->clone(*xCurve);
        
        boost::shared_ptr<Curve> curveBug;
        ar & boost::serialization::make_nvp("Y",curveBug);
     
        
        ar & boost::serialization::make_nvp("Y_animation",yCurve);
        _imp->curveY->clone(*yCurve);
        
        ar & boost::serialization::make_nvp("Left_X",_imp->leftX);
        
        
        ar & boost::serialization::make_nvp("Left_X_animation",leftCurveX);
        ar & boost::serialization::make_nvp("Left_Y",_imp->leftY);
        ar & boost::serialization::make_nvp("Left_Y_animation",leftCurveY);
        ar & boost::serialization::make_nvp("Right_X",_imp->rightX);
        ar & boost::serialization::make_nvp("Right_X_animation",rightCurveX);
        ar & boost::serialization::make_nvp("Right_Y",_imp->rightY);
        ar & boost::serialization::make_nvp("Right_Y_animation",rightCurveY);
        
        _imp->curveLeftBezierX->clone(*leftCurveX);
        _imp->curveLeftBezierY->clone(*leftCurveY);
        _imp->curveRightBezierX->clone(*rightCurveX);
        _imp->curveRightBezierY->clone(*rightCurveY);
    }
    if (version >= BEZIER_CP_INTRODUCES_OFFSET) {
        QWriteLocker l(&_imp->masterMutex);
        ar & boost::serialization::make_nvp("OffsetTime",_imp->offsetTime);
    }
}

BOOST_CLASS_VERSION(BezierCP,BEZIER_CP_VERSION)

class RotoItemSerialization
{
    friend class boost::serialization::access;
    friend class RotoItem;

public:

    RotoItemSerialization()
    : name()
    , activated(false)
    , parentLayerName()
    , locked(false)
    {
    }

    virtual ~RotoItemSerialization()
    {
    }

private:


    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;
        ar & boost::serialization::make_nvp("Name",name);
        ar & boost::serialization::make_nvp("Label",label);
        ar & boost::serialization::make_nvp("Activated",activated);
        ar & boost::serialization::make_nvp("ParentLayer",parentLayerName);
        ar & boost::serialization::make_nvp("Locked",locked);
    }

    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {
        (void)version;
        ar & boost::serialization::make_nvp("Name",name);
        if ( version >= ROTO_ITEM_INTRODUCES_LABEL) {
            ar & boost::serialization::make_nvp("Label",label);
        }
        ar & boost::serialization::make_nvp("Activated",activated);
        ar & boost::serialization::make_nvp("ParentLayer",parentLayerName);
        ar & boost::serialization::make_nvp("Locked",locked);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    std::string name,label;
    bool activated;
    std::string parentLayerName;
    bool locked;
};

BOOST_CLASS_VERSION(RotoItemSerialization,ROTO_ITEM_VERSION)


//BOOST_SERIALIZATION_ASSUME_ABSTRACT(RotoItemSerialization);

class RotoDrawableItemSerialization
    : public RotoItemSerialization
{
    friend class boost::serialization::access;
    friend class RotoDrawableItem;

public:

    RotoDrawableItemSerialization()
        : RotoItemSerialization()
          , _hasColorAndCompOp(false)
    {
    }

    virtual ~RotoDrawableItemSerialization()
    {
    }

private:


    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;
        boost::serialization::void_cast_register<RotoDrawableItemSerialization,RotoItemSerialization>(
            static_cast<RotoDrawableItemSerialization *>(NULL),
            static_cast<RotoItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoItemSerialization);
        ar & boost::serialization::make_nvp("Activated",_activated);
        ar & boost::serialization::make_nvp("Opacity",_opacity);
        ar & boost::serialization::make_nvp("Feather",_feather);
        ar & boost::serialization::make_nvp("FallOff",_featherFallOff);
#ifdef NATRON_ROTO_INVERTIBLE
        ar & boost::serialization::make_nvp("Inverted",_inverted);
#endif
        ar & boost::serialization::make_nvp("Color",_color);
        ar & boost::serialization::make_nvp("CompOP",_compOp);
        ar & boost::serialization::make_nvp("OC.r",_overlayColor[0]);
        ar & boost::serialization::make_nvp("OC.g",_overlayColor[1]);
        ar & boost::serialization::make_nvp("OC.b",_overlayColor[2]);
        ar & boost::serialization::make_nvp("OC.a",_overlayColor[3]);
    }

    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {
        (void)version;
        boost::serialization::void_cast_register<RotoDrawableItemSerialization,RotoItemSerialization>(
            static_cast<RotoDrawableItemSerialization *>(NULL),
            static_cast<RotoItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoItemSerialization);
        ar & boost::serialization::make_nvp("Activated",_activated);
        ar & boost::serialization::make_nvp("Opacity",_opacity);
        ar & boost::serialization::make_nvp("Feather",_feather);
        ar & boost::serialization::make_nvp("FallOff",_featherFallOff);
        if (version < ROTO_DRAWABLE_ITEM_REMOVES_INVERTED) {
            KnobSerialization invertedSerialization;
            ar & boost::serialization::make_nvp("Inverted",invertedSerialization);
#ifdef NATRON_ROTO_INVERTIBLE
            _inverted = invertedSerialization;
#endif
        }
        if (version >= ROTO_DRAWABLE_ITEM_INTRODUCES_COMPOSITING) {
            _hasColorAndCompOp = true;
            ar & boost::serialization::make_nvp("Color",_color);
            ar & boost::serialization::make_nvp("CompOP",_compOp);
        } else {
            _hasColorAndCompOp = false;
        }
        ar & boost::serialization::make_nvp("OC.r",_overlayColor[0]);
        ar & boost::serialization::make_nvp("OC.g",_overlayColor[1]);
        ar & boost::serialization::make_nvp("OC.b",_overlayColor[2]);
        ar & boost::serialization::make_nvp("OC.a",_overlayColor[3]);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    KnobSerialization _activated;
    KnobSerialization _opacity;
    KnobSerialization _feather;
    KnobSerialization _featherFallOff;
#ifdef NATRON_ROTO_INVERTIBLE
    KnobSerialization _inverted;
#endif

    bool _hasColorAndCompOp;
    KnobSerialization _color;
    KnobSerialization _compOp;
    double _overlayColor[4];
};

BOOST_CLASS_VERSION(RotoDrawableItemSerialization,ROTO_DRAWABLE_ITEM_VERSION)

BOOST_SERIALIZATION_ASSUME_ABSTRACT(RotoDrawableItemSerialization);


struct StrokePoint {
    
    double x,y,pressure;
    
    template<class Archive>
    void serialize(Archive & ar,
              const unsigned int /*version*/)
    {
        ar & boost::serialization::make_nvp("X",x);
        ar & boost::serialization::make_nvp("Y",y);
        ar & boost::serialization::make_nvp("Press",pressure);
    }
};


class BezierSerialization
    : public RotoDrawableItemSerialization
{
    friend class boost::serialization::access;
    friend class Bezier;
    
public:
    
    BezierSerialization()
    : RotoDrawableItemSerialization()
    , _controlPoints()
    , _featherPoints()
    , _isStroke(false)
    , _closed(false)
    {
    }

    virtual ~BezierSerialization()
    {
    }

private:


    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;
        boost::serialization::void_cast_register<BezierSerialization,RotoDrawableItemSerialization>(
            static_cast<BezierSerialization *>(NULL),
            static_cast<RotoDrawableItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoDrawableItemSerialization);
        assert( _controlPoints.size() == _featherPoints.size() || _featherPoints.empty());
        int numPoints = (int)_controlPoints.size();
        ar & boost::serialization::make_nvp("NumPoints",numPoints);
        ar & boost::serialization::make_nvp("IsStroke",_isStroke);
        std::list< BezierCP >::const_iterator itF = _featherPoints.begin();
        for (std::list< BezierCP >::const_iterator it = _controlPoints.begin(); it != _controlPoints.end(); ++it) {
            ar & boost::serialization::make_nvp("CP",*it);
            if (!_isStroke) {
                ar & boost::serialization::make_nvp("FP",*itF);
                ++itF;
            }
        }
        ar & boost::serialization::make_nvp("Closed",_closed);
    }

    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {
        (void)version;
        boost::serialization::void_cast_register<BezierSerialization,RotoDrawableItemSerialization>(
            static_cast<BezierSerialization *>(NULL),
            static_cast<RotoDrawableItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoDrawableItemSerialization);
        int numPoints;
        ar & boost::serialization::make_nvp("NumPoints",numPoints);
        if (version >= BEZIER_SERIALIZATION_INTRODUCES_ROTO_STROKE) {
            ar & boost::serialization::make_nvp("IsStroke",_isStroke);
        } else {
            _isStroke = false;
        }
        for (int i = 0; i < numPoints; ++i) {
            BezierCP cp;
            ar & boost::serialization::make_nvp("CP",cp);
            _controlPoints.push_back(cp);
            
            if (!_isStroke) {
                BezierCP fp;
                ar & boost::serialization::make_nvp("FP",fp);
                _featherPoints.push_back(fp);
            }
        }
        ar & boost::serialization::make_nvp("Closed",_closed);
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    std::list< BezierCP > _controlPoints,_featherPoints;
    bool _isStroke;
    bool _closed;
};

BOOST_CLASS_VERSION(BezierSerialization,BEZIER_SERIALIZATION_VERSION)

class RotoStrokeSerialization : public BezierSerialization
{
    friend class boost::serialization::access;
    friend class RotoStrokeItem;
    
public:
    
    
    RotoStrokeSerialization()
    : BezierSerialization()
    , _brushType()
    , _brushSize()
    , _brushHardness()
    , _brushSpacing()
    , _brushVisiblePortion()
    , _brushEffectStrength()
    {
        
    }
    
    virtual ~RotoStrokeSerialization()
    {
        
    }
    
    int getType() const
    {
        return _brushType;
    }
    
private:
    
    
    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;
        boost::serialization::void_cast_register<RotoStrokeSerialization,BezierSerialization>(
                                                                                                        static_cast<RotoStrokeSerialization *>(NULL),
                                                                                                        static_cast<BezierSerialization *>(NULL)
                                                                                                        );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(BezierSerialization);
        ar & boost::serialization::make_nvp("BrushType",_brushType);
        ar & boost::serialization::make_nvp("BrushSize",_brushSize);
        ar & boost::serialization::make_nvp("BrushSpacing",_brushSpacing);
        ar & boost::serialization::make_nvp("BrushHardness",_brushHardness);
        ar & boost::serialization::make_nvp("BrushEffectStrength",_brushEffectStrength);
        ar & boost::serialization::make_nvp("BrushVisiblePortion",_brushVisiblePortion);
#ifndef ROTO_STROKE_USE_FIT_CURVE
        ar & boost::serialization::make_nvp("CurveX",_xCurve);
        ar & boost::serialization::make_nvp("CurveY",_yCurve);
        ar & boost::serialization::make_nvp("CurveP",_pressureCurve);
#endif
    }
    
    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {
        (void)version;
        boost::serialization::void_cast_register<RotoStrokeSerialization,BezierSerialization>(
                                                                                                        static_cast<RotoStrokeSerialization *>(NULL),
                                                                                                        static_cast<BezierSerialization *>(NULL)
                                                                                                        );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(BezierSerialization);
        ar & boost::serialization::make_nvp("BrushType",_brushType);
        ar & boost::serialization::make_nvp("BrushSize",_brushSize);
        ar & boost::serialization::make_nvp("BrushSpacing",_brushSpacing);
        ar & boost::serialization::make_nvp("BrushHardness",_brushHardness);
        ar & boost::serialization::make_nvp("BrushEffectStrength",_brushEffectStrength);
        ar & boost::serialization::make_nvp("BrushVisiblePortion",_brushVisiblePortion);
#ifndef ROTO_STROKE_USE_FIT_CURVE
        ar & boost::serialization::make_nvp("CurveX",_xCurve);
        ar & boost::serialization::make_nvp("CurveY",_yCurve);
        ar & boost::serialization::make_nvp("CurveP",_pressureCurve);
#endif
    }
    
    BOOST_SERIALIZATION_SPLIT_MEMBER()
    
    int _brushType;
    KnobSerialization _brushSize;
    KnobSerialization _brushHardness;
    KnobSerialization _brushSpacing;
    KnobSerialization _brushVisiblePortion;
    KnobSerialization _brushEffectStrength;
#ifndef ROTO_STROKE_USE_FIT_CURVE
    Curve _xCurve,_yCurve,_pressureCurve;
#endif
};

class RotoLayerSerialization
    : public RotoItemSerialization
{
    friend class boost::serialization::access;
    friend class RotoLayer;

public:

    RotoLayerSerialization()
        : RotoItemSerialization()
    {
    }

    virtual ~RotoLayerSerialization()
    {
    }

private:


    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;

        boost::serialization::void_cast_register<RotoLayerSerialization,RotoItemSerialization>(
            static_cast<RotoLayerSerialization *>(NULL),
            static_cast<RotoItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoItemSerialization);
        int numChildren = (int)children.size();
        ar & boost::serialization::make_nvp("NumChildren",numChildren);
        for (std::list < boost::shared_ptr<RotoItemSerialization> >::const_iterator it = children.begin(); it != children.end(); ++it) {
            BezierSerialization* isBezier = dynamic_cast<BezierSerialization*>( it->get() );
            RotoStrokeSerialization* isStroke = dynamic_cast<RotoStrokeSerialization*>( it->get() );
            RotoLayerSerialization* isLayer = dynamic_cast<RotoLayerSerialization*>( it->get() );
            int type = 0;
            if (isBezier && !isStroke) {
                type = 0;
            } else if (isStroke) {
                type = 1;
            } else if (isLayer) {
                type = 2;
            }
            ar & boost::serialization::make_nvp("Type",type);
            if (isBezier && !isStroke) {
                ar & boost::serialization::make_nvp("Item",*isBezier);
            } else if (isStroke) {
                ar & boost::serialization::make_nvp("Item",*isStroke);
            } else {
                assert(isLayer);
                ar & boost::serialization::make_nvp("Item",*isLayer);
            }
        }
    }

    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {
        (void)version;
        boost::serialization::void_cast_register<RotoLayerSerialization,RotoItemSerialization>(
            static_cast<RotoLayerSerialization *>(NULL),
            static_cast<RotoItemSerialization *>(NULL)
            );
        ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(RotoItemSerialization);
        int numChildren;
        ar & boost::serialization::make_nvp("NumChildren",numChildren);
        for (int i = 0; i < numChildren; ++i) {
            
            int type;
            if (version < ROTO_LAYER_SERIALIZATION_REMOVES_IS_BEZIER) {
                bool bezier;
                ar & boost::serialization::make_nvp("IsBezier",bezier);
                type = 0;
            } else {
                ar & boost::serialization::make_nvp("Type",type);
            }
            if (type == 0) {
                boost::shared_ptr<BezierSerialization> b(new BezierSerialization);
                ar & boost::serialization::make_nvp("Item",*b);
                children.push_back(b);
            } else if (type == 1) {
                boost::shared_ptr<RotoStrokeSerialization> b(new RotoStrokeSerialization);
                ar & boost::serialization::make_nvp("Item",*b);
                children.push_back(b);
            } else {
                boost::shared_ptr<RotoLayerSerialization> l(new RotoLayerSerialization);
                ar & boost::serialization::make_nvp("Item",*l);
                children.push_back(l);
            }
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    std::list < boost::shared_ptr<RotoItemSerialization> > children;
};

BOOST_CLASS_VERSION(RotoLayerSerialization,ROTO_LAYER_SERIALIZATION_VERSION)

class RotoContextSerialization
{
    friend class boost::serialization::access;
    friend class RotoContext;

public:

    RotoContextSerialization()
        : _baseLayer()
        , _selectedItems()
        , _autoKeying(false)
        , _rippleEdit(false)
        , _featherLink(false)
    {
    }

    ~RotoContextSerialization()
    {
    }

private:


    template<class Archive>
    void save(Archive & ar,
              const unsigned int version) const
    {
        (void)version;

        ar & boost::serialization::make_nvp("BaseLayer",_baseLayer);
        ar & boost::serialization::make_nvp("AutoKeying",_autoKeying);
        ar & boost::serialization::make_nvp("RippleEdit",_rippleEdit);
        ar & boost::serialization::make_nvp("FeatherLink",_featherLink);
        ar & boost::serialization::make_nvp("Selection",_selectedItems);
    }

    template<class Archive>
    void load(Archive & ar,
              const unsigned int version)
    {

        ar & boost::serialization::make_nvp("BaseLayer",_baseLayer);
        ar & boost::serialization::make_nvp("AutoKeying",_autoKeying);
        ar & boost::serialization::make_nvp("RippleEdit",_rippleEdit);
        ar & boost::serialization::make_nvp("FeatherLink",_featherLink);
        ar & boost::serialization::make_nvp("Selection",_selectedItems);
        if (version < ROTO_CTX_REMOVE_COUNTERS) {
            std::map<std::string,int> _itemCounters;
            ar & boost::serialization::make_nvp("Counters",_itemCounters);
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()

    RotoLayerSerialization _baseLayer;
    std::list< std::string > _selectedItems;
    bool _autoKeying;
    bool _rippleEdit;
    bool _featherLink;
};

BOOST_CLASS_VERSION(RotoContextSerialization,ROTO_CTX_VERSION)


#endif // ROTOSERIALIZATION_H
