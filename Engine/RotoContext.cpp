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

// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>

#include "RotoContext.h"

#include <algorithm>
#include <sstream>
#include <locale>
#include <limits>

#include <QLineF>

#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>

#include "Global/MemoryInfo.h"
#include "Engine/RotoContextPrivate.h"

#include "Engine/Interpolation.h"
#include "Engine/AppInstance.h"
#include "Engine/TimeLine.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/Hash64.h"
#include "Engine/Settings.h"
#include "Engine/Format.h"
#include "Engine/RotoSerialization.h"
#include "Engine/Transform.h"
#include "Engine/CoonsRegularization.h"

//This will enable correct evaluation of beziers
//#define ROTO_USE_MESH_PATTERN_ONLY

using namespace Natron;



static inline double
lerp(double a,
     double b,
     double t)
{
    return a + (b - a) * t;
}

static inline void
lerpPoint(const Point & a,
          const Point & b,
          double t,
          Point *dest)
{
    dest->x = lerp(a.x, b.x, t);
    dest->y = lerp(a.y, b.y, t);
}

// compute value using the de Casteljau recursive formula
static inline double
bezier(double p0,
       double p1,
       double p2,
       double p3,
       double t)
{
    double p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3;
    
    p0p1 = lerp(p0, p1, t);
    p1p2 = lerp(p1, p2, t);
    p2p3 = lerp(p2, p3, t);
    p0p1_p1p2 = lerp(p0p1, p1p2, t);
    p1p2_p2p3 = lerp(p1p2, p2p3, t);
    
    return lerp(p0p1_p1p2, p1p2_p2p3, t);
}

// compute point using the de Casteljau recursive formula
static inline void
bezierFullPoint(const Point & p0,
                const Point & p1,
                const Point & p2,
                const Point & p3,
                double t,
                Point *p0p1,
                Point *p1p2,
                Point *p2p3,
                Point *p0p1_p1p2,
                Point *p1p2_p2p3,
                Point *dest)
{
    lerpPoint(p0, p1, t, p0p1);
    lerpPoint(p1, p2, t, p1p2);
    lerpPoint(p2, p3, t, p2p3);
    lerpPoint(*p0p1, *p1p2, t, p0p1_p1p2);
    lerpPoint(*p1p2, *p2p3, t, p1p2_p2p3);
    lerpPoint(*p0p1_p1p2, *p1p2_p2p3, t, dest);
}

void
Bezier::bezierPoint(const Point & p0,
            const Point & p1,
            const Point & p2,
            const Point & p3,
            double t,
            Point *dest)
{
    Point p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3;
    bezierFullPoint(p0, p1, p2, p3, t, &p0p1, &p1p2, &p2p3, &p0p1_p1p2, &p1p2_p2p3, dest);
}

#if 0 //UNUSED CODE
// compute polynomial coefficients so that
// P(t) = A*t^3 + B*t^2 + C*t + D
static inline void
bezierPolyCoeffs(double p0,
                 double p1,
                 double p2,
                 double p3,
                 double *a,
                 double *b,
                 double *c,
                 double *d)
{
    // d = P0
    *d = p0;
    // c = 3*P1-3*P0
    *c = 3 * p1 - 3 * p0;
    // b = 3*P2-6*P1+3*P0
    *b = 3 * p2 - 6 * p1 + 3 * p0;
    // a = P3-3*P2+3*P1-P0
    *a = p3 - 3 * p2 + 3 * p1 - p0;
}

#endif

// compute polynomial coefficients so that
// P'(t) = A*t^2 + B*t + C
static inline void
bezierPolyDerivativeCoeffs(double p0,
                           double p1,
                           double p2,
                           double p3,
                           double *a,
                           double *b,
                           double *c)
{
    // c = 3*P1-3*P0
    *c = 3 * p1 - 3 * p0;
    // b = 2*(3*P2-6*P1+3*P0)
    *b = 2 * (3 * p2 - 6 * p1 + 3 * p0);
    // a = 3*(P3-3*P2+3*P1-P0)
    *a = 3 * (p3 - 3 * p2 + 3 * p1 - p0);
}

static inline void
updateRange(double x,
            double *xmin,
            double *xmax)
{
    if (x < *xmin) {
        *xmin = x;
    }
    if (x > *xmax) {
        *xmax = x;
    }
}

// compute the bounds of the Bezier for t \in [0,1]
// algorithm:
// - compute extrema of the cubic, i.e. values of t for
// which the derivative of the x coordinate of the
// Bezier is 0. If they are in [0,1] then they take part in
// range computation (there can be up to two extrema). the
// Bbox is the Bbox of these points and the
// extremal points (P0,P3)
static inline void
bezierBounds(double p0,
             double p1,
             double p2,
             double p3,
             double *xmin,
             double *xmax)
{
    // initialize with the range of the endpoints
    *xmin = std::min(p0, p3);
    *xmax = std::max(p0, p3);
    double a, b, c;
    bezierPolyDerivativeCoeffs(p0, p1, p2, p3, &a, &b, &c);
    if (a == 0) {
        //aX^2 + bX + c well then then this is a simple line
        //x= -c / b
        double t = -c / b;
        if ( (0 < t) && (t < 1) ) {
            updateRange(bezier(p0, p1, p2, p3, t), xmin, xmax);
        }
        
        return;
    }
    double disc = b * b - 4 * a * c;
    if (disc < 0) {
        // no real solution
    } else if (disc == 0) {
        double t = -b / (2 * a);
        if ( (0 < t) && (t < 1) ) {
            updateRange(bezier(p0, p1, p2, p3, t), xmin, xmax);
        }
    } else {
        double t;
        t = ( -b - std::sqrt(disc) ) / (2 * a);
        if ( (0 < t) && (t < 1) ) {
            updateRange(bezier(p0, p1, p2, p3, t), xmin, xmax);
        }
        t = ( -b + std::sqrt(disc) ) / (2 * a);
        if ( (0 < t) && (t < 1) ) {
            updateRange(bezier(p0, p1, p2, p3, t), xmin, xmax);
        }
    }
}

// updates param bbox with the bbox of this segment
static void
bezierPointBboxUpdate(const Point & p0,
                      const Point & p1,
                      const Point & p2,
                      const Point & p3,
                      RectD *bbox) ///< input/output
{
    {
        double x1, x2;
        bezierBounds(p0.x, p1.x, p2.x, p3.x, &x1, &x2);
        if (x1 < bbox->x1) {
            bbox->x1 = x1;
        }
        if (x2 > bbox->x2) {
            bbox->x2 = x2;
        }
    }
    {
        double y1, y2;
        bezierBounds(p0.y, p1.y, p2.y, p3.y, &y1, &y2);
        if (y1 < bbox->y1) {
            bbox->y1 = y1;
        }
        if (y2 > bbox->y2) {
            bbox->y2 = y2;
        }
    }
}

// compute a bounding box for the bezier segment
// algorithm:
// - compute extrema of the cubic, i.e. values of t for
// which the derivative of the x or y coordinate of the
// Bezier is 0. If they are in [0,1] then they take part in
// bbox computation (there can be up to four extrema, 2 for
// x and 2 for y). the Bbox is the Bbox of these points and the
// extremal points (P0,P3)
static void
bezierSegmentBboxUpdate(const BezierCP & first,
                        const BezierCP & last,
                        int time,
                        unsigned int mipMapLevel,
                        RectD* bbox) ///< input/output
{
    Point p0,p1,p2,p3;
    
    assert(bbox);
    
    try {
        first.getPositionAtTime(time, &p0.x, &p0.y);
        first.getRightBezierPointAtTime(time, &p1.x, &p1.y);
        last.getPositionAtTime(time, &p3.x, &p3.y);
        last.getLeftBezierPointAtTime(time, &p2.x, &p2.y);
    } catch (const std::exception & e) {
        assert(false);
    }
    
    if (mipMapLevel > 0) {
        int pot = 1 << mipMapLevel;
        p0.x /= pot;
        p0.y /= pot;
        
        p1.x /= pot;
        p1.y /= pot;
        
        p2.x /= pot;
        p2.y /= pot;
        
        p3.x /= pot;
        p3.y /= pot;
    }
    bezierPointBboxUpdate(p0, p1, p2, p3, bbox);
}

static void
bezierSegmentListBboxUpdate(const BezierCPs & points,
                            bool finished,
                            int time,
                            unsigned int mipMapLevel,
                            RectD* bbox) ///< input/output
{
    if ( points.empty() ) {
        return;
    }
    if (points.size() == 1) {
        // only one point
        Point p0;
        const boost::shared_ptr<BezierCP>& p = points.front();
        p->getPositionAtTime(time, &p0.x, &p0.y);
    }
    BezierCPs::const_iterator next = points.begin();
    if (next != points.end()) {
        ++next;
    }
    for (BezierCPs::const_iterator it = points.begin(); it != points.end(); ++it) {
        if ( next == points.end() ) {
            if (!finished) {
                break;
            }
            next = points.begin();
        }
        bezierSegmentBboxUpdate(*(*it), *(*next), time, mipMapLevel, bbox);
        
        // increment for next iteration
        if (next != points.end()) {
            ++next;
        }
    } // for()
}

// compute nbPointsperSegment points and update the bbox bounding box for the Bezier
// segment from 'first' to 'last' evaluated at 'time'
// If nbPointsPerSegment is -1 then it will be automatically computed
static void
bezierSegmentEval(const BezierCP & first,
                  const BezierCP & last,
                  int time,
                  unsigned int mipMapLevel,
                  int nbPointsPerSegment,
                  std::list< Point >* points, ///< output
                  RectD* bbox = NULL) ///< input/output (optional)
{
    Point p0,p1,p2,p3;
    
    try {
        first.getPositionAtTime(time, &p0.x, &p0.y);
        first.getRightBezierPointAtTime(time, &p1.x, &p1.y);
        last.getPositionAtTime(time, &p3.x, &p3.y);
        last.getLeftBezierPointAtTime(time, &p2.x, &p2.y);
    } catch (const std::exception & e) {
        assert(false);
    }
    
    if (mipMapLevel > 0) {
        int pot = 1 << mipMapLevel;
        p0.x /= pot;
        p0.y /= pot;
        
        p1.x /= pot;
        p1.y /= pot;
        
        p2.x /= pot;
        p2.y /= pot;
        
        p3.x /= pot;
        p3.y /= pot;
    }
    
    if (nbPointsPerSegment == -1) {
        /*
         * Approximate the necessary number of line segments, using http://antigrain.com/research/adaptive_bezier/
         */
        double dx1,dy1,dx2,dy2,dx3,dy3;
        dx1 = p1.x - p0.x;
        dy1 = p1.y - p0.y;
        dx2 = p2.x - p1.x;
        dy2 = p2.y - p1.y;
        dx3 = p3.x - p2.x;
        dy3 = p3.y - p2.y;
        double length = std::sqrt(dx1 * dx1 + dy1 * dy1) +
        std::sqrt(dx2 * dx2 + dy2 * dy2) +
        std::sqrt(dx3 * dx3 + dy3 * dy3);
        nbPointsPerSegment = (int)std::max(length * 0.25, 2.);
    }
    
    double incr = 1. / (double)(nbPointsPerSegment - 1);
    Point cur;
    for (double t = 0.; t <= 1.; t += incr) {
        Bezier::bezierPoint(p0, p1, p2, p3, t, &cur);
        points->push_back(cur);
    }
    if (bbox) {
        bezierPointBboxUpdate(p0,  p1,  p2,  p3, bbox);
    }
}

/**
 * @brief Determines if the point (x,y) lies on the bezier curve segment defined by first and last.
 * @returns True if the point is close (according to the acceptance) to the curve, false otherwise.
 * @param param[out] It is set to the parametric value at which the subdivision of the bezier segment
 * yields the closest point to (x,y) on the curve.
 **/
static bool
bezierSegmentMeetsPoint(const BezierCP & first,
                        const BezierCP & last,
                        int time,
                        double x,
                        double y,
                        double distance,
                        double *param) ///< output
{
    Point p0,p1,p2,p3;
    
    first.getPositionAtTime(time, &p0.x, &p0.y);
    first.getRightBezierPointAtTime(time, &p1.x, &p1.y);
    last.getPositionAtTime(time, &p3.x, &p3.y);
    last.getLeftBezierPointAtTime(time, &p2.x, &p2.y);
    
    ///Use the control polygon to approximate segment length
    double length = ( std::sqrt( (p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y) ) +
                     std::sqrt( (p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y) ) +
                     std::sqrt( (p3.x - p2.x) * (p3.x - p2.x) + (p3.y - p2.y) * (p3.y - p2.y) ) );
    // increment is the distance divided by the  segment length
    double incr = length == 0. ? 1. : distance / length;
    
    ///the minimum square distance between a decasteljau point an the given (x,y) point
    ///we save a sqrt call
    double sqDistance = distance * distance;
    double minSqDistance = std::numeric_limits<double>::infinity();
    double tForMin = -1.;
    for (double t = 0.; t <= 1.; t += incr) {
        Point p;
        Bezier::bezierPoint(p0, p1, p2, p3, t, &p);
        double sqdist = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y);
        if ( (sqdist <= sqDistance) && (sqdist < minSqDistance) ) {
            minSqDistance = sqdist;
            tForMin = t;
        }
    }
    
    if (minSqDistance <= sqDistance) {
        *param = tForMin;
        
        return true;
    }
    
    return false;
}

static bool
isPointCloseTo(int time,
               const BezierCP & p,
               double x,
               double y,
               double acceptance)
{
    double px,py;
    
    p.getPositionAtTime(time, &px, &py);
    if ( ( px >= (x - acceptance) ) && ( px <= (x + acceptance) ) && ( py >= (y - acceptance) ) && ( py <= (y + acceptance) ) ) {
        return true;
    }
    
    return false;
}

static bool
bezierSegmenEqual(int time,
                  const BezierCP & p0,
                  const BezierCP & p1,
                  const BezierCP & s0,
                  const BezierCP & s1)
{
    double prevX,prevY,prevXF,prevYF;
    double nextX,nextY,nextXF,nextYF;
    
    p0.getPositionAtTime(time, &prevX, &prevY);
    p1.getPositionAtTime(time, &nextX, &nextY);
    s0.getPositionAtTime(time, &prevXF, &prevYF);
    s1.getPositionAtTime(time, &nextXF, &nextYF);
    if ( (prevX != prevXF) || (prevY != prevYF) || (nextX != nextXF) || (nextY != nextYF) ) {
        return true;
    } else {
        ///check derivatives
        double prevRightX,prevRightY,nextLeftX,nextLeftY;
        double prevRightXF,prevRightYF,nextLeftXF,nextLeftYF;
        p0.getRightBezierPointAtTime(time, &prevRightX, &prevRightY);
        p1.getLeftBezierPointAtTime(time, &nextLeftX, &nextLeftY);
        s0.getRightBezierPointAtTime(time,&prevRightXF, &prevRightYF);
        s1.getLeftBezierPointAtTime(time, &nextLeftXF, &nextLeftYF);
        if ( (prevRightX != prevRightXF) || (prevRightY != prevRightYF) || (nextLeftX != nextLeftXF) || (nextLeftY != nextLeftYF) ) {
            return true;
        } else {
            return false;
        }
    }
}

////////////////////////////////////ControlPoint////////////////////////////////////

BezierCP::BezierCP()
    : _imp( new BezierCPPrivate(boost::shared_ptr<Bezier>()) )
{
}

BezierCP::BezierCP(const BezierCP & other)
    : _imp( new BezierCPPrivate(other._imp->holder.lock()) )
{
    clone(other);
}

BezierCP::BezierCP(const boost::shared_ptr<Bezier>& curve)
    : _imp( new BezierCPPrivate(curve) )
{
}

BezierCP::~BezierCP()
{
}

bool
BezierCP::getPositionAtTime(int time,
                            double* x,
                            double* y,
                            bool skipMasterOrRelative) const
{
    bool ret;
    KeyFrame k;

    if ( _imp->curveX->getKeyFrameWithTime(time, &k) ) {
        bool ok;
        *x = k.getValue();
        ok = _imp->curveY->getKeyFrameWithTime(time, &k);
        assert(ok);
        *y = k.getValue();
        ret = true;
    } else {
        try {
            *x = _imp->curveX->getValueAt(time);
            *y = _imp->curveY->getValueAt(time);
        } catch (const std::exception & e) {
            QMutexLocker l(&_imp->staticPositionMutex);
            *x = _imp->x;
            *y = _imp->y;
        }

        ret = false;
    }

    if (!skipMasterOrRelative) {
        SequenceTime offsetTime;
        Double_Knob* masterTrack;
        {
            QReadLocker l(&_imp->masterMutex);
            offsetTime = _imp->offsetTime;
            masterTrack = _imp->masterTrack ? _imp->masterTrack.get() : NULL;
        }
        if (masterTrack) {
            double masterX = masterTrack->getValueAtTime(time,0);
            double masterY = masterTrack->getValueAtTime(time,1);
            double masterOffsetTimeX = masterTrack->getValueAtTime(offsetTime,0);
            double masterOffsetTimeY = masterTrack->getValueAtTime(offsetTime,1);
            *x += (masterX - masterOffsetTimeX);
            *y += (masterY - masterOffsetTimeY);
        }
    }

    return ret;
}

void
BezierCP::setPositionAtTime(int time,
                            double x,
                            double y)
{
    {
        KeyFrame k(time,x);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveX->addKeyFrame(k);
    }
    {
        KeyFrame k(time,y);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveY->addKeyFrame(k);
    }
}

void
BezierCP::setStaticPosition(double x,
                            double y)
{
    QMutexLocker l(&_imp->staticPositionMutex);
    _imp->x = x;
    _imp->y = y;
}

void
BezierCP::setLeftBezierStaticPosition(double x,
                                      double y)
{
    QMutexLocker l(&_imp->staticPositionMutex);
    _imp->leftX = x;
    _imp->leftY = y;
}

void
BezierCP::setRightBezierStaticPosition(double x,
                                       double y)
{
    QMutexLocker l(&_imp->staticPositionMutex);
    _imp->rightX = x;
    _imp->rightY = y;
}

bool
BezierCP::getLeftBezierPointAtTime(int time,
                                   double* x,
                                   double* y,
                                   bool skipMasterOrRelative) const
{
    KeyFrame k;
    bool ret;

    if ( _imp->curveLeftBezierX->getKeyFrameWithTime(time, &k) ) {
        bool ok;
        *x = k.getValue();
        ok = _imp->curveLeftBezierY->getKeyFrameWithTime(time, &k);
        assert(ok);
        *y = k.getValue();
        ret =  true;
    } else {
        try {
            *x = _imp->curveLeftBezierX->getValueAt(time);
            *y = _imp->curveLeftBezierY->getValueAt(time);
        } catch (const std::exception & e) {
            QMutexLocker l(&_imp->staticPositionMutex);
            *x = _imp->leftX;
            *y = _imp->leftY;
        }

        ret =  false;
    }

    if (!skipMasterOrRelative) {
        Double_Knob* masterTrack;
        SequenceTime offsetTime;
        {
            QReadLocker l(&_imp->masterMutex);
            masterTrack = _imp->masterTrack ? _imp->masterTrack.get() : NULL;
            offsetTime = _imp->offsetTime;
        }
        if (masterTrack) {
            double masterX = masterTrack->getValueAtTime(time,0);
            double masterY = masterTrack->getValueAtTime(time,1);
            double masterOffsetTimeX = masterTrack->getValueAtTime(offsetTime,0);
            double masterOffsetTimeY = masterTrack->getValueAtTime(offsetTime,1);
            *x += (masterX - masterOffsetTimeX);
            *y += (masterY - masterOffsetTimeY);
        }
    }

    return ret;
}

bool
BezierCP::getRightBezierPointAtTime(int time,
                                    double *x,
                                    double *y,
                                    bool skipMasterOrRelative) const
{
    KeyFrame k;
    bool ret;

    if ( _imp->curveRightBezierX->getKeyFrameWithTime(time, &k) ) {
        bool ok;
        *x = k.getValue();
        ok = _imp->curveRightBezierY->getKeyFrameWithTime(time, &k);
        assert(ok);
        *y = k.getValue();
        ret = true;
    } else {
        try {
            *x = _imp->curveRightBezierX->getValueAt(time);
            *y = _imp->curveRightBezierY->getValueAt(time);
        } catch (const std::exception & e) {
            QMutexLocker l(&_imp->staticPositionMutex);
            *x = _imp->rightX;
            *y = _imp->rightY;
        }

        ret =  false;
    }


    if (!skipMasterOrRelative) {
        Double_Knob* masterTrack;
        SequenceTime offsetTime;
        {
            QReadLocker l(&_imp->masterMutex);
            masterTrack = _imp->masterTrack ? _imp->masterTrack.get() : NULL;
            offsetTime = _imp->offsetTime;
        }
        if (masterTrack) {
            double masterX = masterTrack->getValueAtTime(time,0);
            double masterY = masterTrack->getValueAtTime(time,1);
            double masterOffsetTimeX = masterTrack->getValueAtTime(offsetTime,0);
            double masterOffsetTimeY = masterTrack->getValueAtTime(offsetTime,1);
            *x += (masterX - masterOffsetTimeX);
            *y += (masterY - masterOffsetTimeY);
        }
    }

    return ret;
}

void
BezierCP::setLeftBezierPointAtTime(int time,
                                   double x,
                                   double y)
{
    {
        KeyFrame k(time,x);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveLeftBezierX->addKeyFrame(k);
    }
    {
        KeyFrame k(time,y);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveLeftBezierY->addKeyFrame(k);
    }
}

void
BezierCP::setRightBezierPointAtTime(int time,
                                    double x,
                                    double y)
{
    {
        KeyFrame k(time,x);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveRightBezierX->addKeyFrame(k);
    }
    {
        KeyFrame k(time,y);
        k.setInterpolation(Natron::eKeyframeTypeLinear);
        _imp->curveRightBezierY->addKeyFrame(k);
    }
}


void
BezierCP::removeAnimation(int currentTime)
{
    {
        QMutexLocker k(&_imp->staticPositionMutex);
        _imp->x = _imp->curveX->getValueAt(currentTime);
        _imp->y = _imp->curveY->getValueAt(currentTime);
        _imp->leftX = _imp->curveLeftBezierX->getValueAt(currentTime);
        _imp->leftY = _imp->curveLeftBezierY->getValueAt(currentTime);
        _imp->rightX = _imp->curveRightBezierX->getValueAt(currentTime);
        _imp->rightY = _imp->curveRightBezierY->getValueAt(currentTime);
    }
    _imp->curveX->clearKeyFrames();
    _imp->curveY->clearKeyFrames();
    _imp->curveLeftBezierX->clearKeyFrames();
    _imp->curveRightBezierX->clearKeyFrames();
    _imp->curveLeftBezierY->clearKeyFrames();
    _imp->curveRightBezierY->clearKeyFrames();
}

void
BezierCP::removeKeyframe(int time)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    ///if the keyframe count reaches 0 update the "static" values which may be fetched
    if (_imp->curveX->getKeyFramesCount() == 1) {
        QMutexLocker l(&_imp->staticPositionMutex);
        _imp->x = _imp->curveX->getValueAt(time);
        _imp->y = _imp->curveY->getValueAt(time);
        _imp->leftX = _imp->curveLeftBezierX->getValueAt(time);
        _imp->leftY = _imp->curveLeftBezierY->getValueAt(time);
        _imp->rightX = _imp->curveRightBezierX->getValueAt(time);
        _imp->rightY = _imp->curveRightBezierY->getValueAt(time);
    }

    try {
        _imp->curveX->removeKeyFrameWithTime(time);
        _imp->curveY->removeKeyFrameWithTime(time);
        _imp->curveLeftBezierX->removeKeyFrameWithTime(time);
        _imp->curveRightBezierX->removeKeyFrameWithTime(time);
        _imp->curveLeftBezierY->removeKeyFrameWithTime(time);
        _imp->curveRightBezierY->removeKeyFrameWithTime(time);
    } catch (...) {
    }
}



bool
BezierCP::hasKeyFrameAtTime(int time) const
{
    KeyFrame k;

    return _imp->curveX->getKeyFrameWithTime(time, &k);
}

void
BezierCP::getKeyframeTimes(std::set<int>* times) const
{
    KeyFrameSet set = _imp->curveX->getKeyFrames_mt_safe();

    for (KeyFrameSet::iterator it = set.begin(); it != set.end(); ++it) {
        times->insert( (int)it->getTime() );
    }
}

void
BezierCP::getKeyFrames(std::list<std::pair<int,Natron::KeyframeTypeEnum> >* keys) const
{
    KeyFrameSet set = _imp->curveX->getKeyFrames_mt_safe();
    for (KeyFrameSet::iterator it = set.begin(); it != set.end(); ++it) {
        keys->push_back(std::make_pair(it->getTime(), it->getInterpolation()));
    }
}

int
BezierCP::getKeyFrameIndex(double time) const
{
    return _imp->curveX->keyFrameIndex(time);
}

void
BezierCP::setKeyFrameInterpolation(Natron::KeyframeTypeEnum interp,int index)
{
    _imp->curveX->setKeyFrameInterpolation(interp, index);
    _imp->curveY->setKeyFrameInterpolation(interp, index);
    _imp->curveLeftBezierX->setKeyFrameInterpolation(interp, index);
    _imp->curveLeftBezierY->setKeyFrameInterpolation(interp, index);
    _imp->curveRightBezierX->setKeyFrameInterpolation(interp, index);
    _imp->curveRightBezierY->setKeyFrameInterpolation(interp, index);
}

int
BezierCP::getKeyframeTime(int index) const
{
    KeyFrame k;
    bool ok = _imp->curveX->getKeyFrameWithIndex(index, &k);

    if (ok) {
        return k.getTime();
    } else {
        return INT_MAX;
    }
}

int
BezierCP::getKeyframesCount() const
{
    return _imp->curveX->getKeyFramesCount();
}

int
BezierCP::getControlPointsCount() const
{
    boost::shared_ptr<Bezier> b = _imp->holder.lock();
    assert(b);
    return b->getControlPointsCount();
}

boost::shared_ptr<Bezier>
BezierCP::getBezier() const
{
    boost::shared_ptr<Bezier> b = _imp->holder.lock();
    assert(b);
    return b;
}

int
BezierCP::isNearbyTangent(int time,
                          double x,
                          double y,
                          double acceptance) const
{
    double xp,yp,leftX,leftY,rightX,rightY;

    getPositionAtTime(time, &xp, &yp);
    getLeftBezierPointAtTime(time, &leftX, &leftY);
    getRightBezierPointAtTime(time, &rightX, &rightY);

    if ( (xp != leftX) || (yp != leftY) ) {
        if ( ( leftX >= (x - acceptance) ) && ( leftX <= (x + acceptance) ) && ( leftY >= (y - acceptance) ) && ( leftY <= (y + acceptance) ) ) {
            return 0;
        }
    }
    if ( (xp != rightX) || (yp != rightY) ) {
        if ( ( rightX >= (x - acceptance) ) && ( rightX <= (x + acceptance) ) && ( rightY >= (y - acceptance) ) && ( rightY <= (y + acceptance) ) ) {
            return 1;
        }
    }

    return -1;
}

#define TANGENTS_CUSP_LIMIT 25
namespace {
static void
cuspTangent(double x,
            double y,
            double *tx,
            double *ty,
            const std::pair<double,double>& pixelScale)
{
    ///decrease the tangents distance by 1 fourth
    ///if the tangents are equal to the control point, make them 10 pixels long
    double dx = *tx - x;
    double dy = *ty - y;
    double distSquare = dx * dx + dy * dy;

    if (distSquare <= pixelScale.first * pixelScale.second * TANGENTS_CUSP_LIMIT * TANGENTS_CUSP_LIMIT) {
        *tx = x;
        *ty = y;
    } else {
        double newDx = 0.9 * dx ;
        double newDy = 0.9 * dy;
        *tx = x + newDx;
        *ty = y + newDy;
    }
}

static void
smoothTangent(int time,
              bool left,
              const BezierCP* p,
              double x,
              double y,
              double *tx,
              double *ty,
              const std::pair<double,double>& pixelScale)
{
    if ( (x == *tx) && (y == *ty) ) {
        
        const std::list < boost::shared_ptr<BezierCP> > & cps = ( p->isFeatherPoint() ?
                                                                  p->getBezier()->getFeatherPoints() :
                                                                  p->getBezier()->getControlPoints() );

        if (cps.size() == 1) {
            return;
        }

        std::list < boost::shared_ptr<BezierCP> >::const_iterator prev = cps.end();
        if (prev != cps.begin()) {
            --prev;
        }
        std::list < boost::shared_ptr<BezierCP> >::const_iterator next = cps.begin();
        if (next != cps.end()) {
            ++next;
        }

        int index = 0;
        int cpCount = (int)cps.size();
        for (std::list < boost::shared_ptr<BezierCP> >::const_iterator it = cps.begin();
             it != cps.end();
             ++it) {
            if ( prev == cps.end() ) {
                prev = cps.begin();
            }
            if ( next == cps.end() ) {
                next = cps.begin();
            }
            if (it->get() == p) {
                break;
            }

            // increment for next iteration
            if (prev != cps.end()) {
                ++prev;
            }
            if (next != cps.end()) {
                ++next;
            }
            ++index;
        } // for(it)

        assert(index < cpCount);
        (void)cpCount;

        double leftDx,leftDy,rightDx,rightDy;
        Bezier::leftDerivativeAtPoint(time, *p, **prev, &leftDx, &leftDy);
        Bezier::rightDerivativeAtPoint(time, *p, **next, &rightDx, &rightDy);
        double norm = sqrt( (rightDx - leftDx) * (rightDx - leftDx) + (rightDy - leftDy) * (rightDy - leftDy) );
        Point delta;
        ///normalize derivatives by their norm
        if (norm != 0) {
            delta.x = ( (rightDx - leftDx) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.first;
            delta.y = ( (rightDy - leftDy) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.second;
        } else {
            ///both derivatives are the same, use the direction of the left one
            norm = sqrt( (leftDx - x) * (leftDx - x) + (leftDy - y) * (leftDy - y) );
            if (norm != 0) {
                delta.x = ( (rightDx - x) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.first;
                delta.y = ( (leftDy - y) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.second;
            } else {
                ///both derivatives and control point are equal, just use 0
                delta.x = delta.y = 0;
            }
        }

        if (!left) {
            *tx = x + delta.x;
            *ty = y + delta.y;
        } else {
            *tx = x - delta.x;
            *ty = y - delta.y;
        }
    } else {
        ///increase the tangents distance by 1 fourth
        ///if the tangents are equal to the control point, make them 10 pixels long
        double dx = *tx - x;
        double dy = *ty - y;
        double newDx,newDy;
        if ( (dx == 0) && (dy == 0) ) {
            dx = (dx < 0 ? -TANGENTS_CUSP_LIMIT : TANGENTS_CUSP_LIMIT) * pixelScale.first;
            dy = (dy < 0 ? -TANGENTS_CUSP_LIMIT : TANGENTS_CUSP_LIMIT) * pixelScale.second;
        }
        newDx = dx * 1.1;
        newDy = dy * 1.1;

        *tx = x + newDx;
        *ty = y + newDy;
    }
} // smoothTangent
}

bool
BezierCP::cuspPoint(int time,
                    bool autoKeying,
                    bool rippleEdit,
                    const std::pair<double,double>& pixelScale)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        ///update the offset time
        QWriteLocker l(&_imp->masterMutex);
        if (_imp->masterTrack) {
            _imp->offsetTime = time;
        }
    }

    double x,y,leftX,leftY,rightX,rightY;
    getPositionAtTime(time, &x, &y,true);
    getLeftBezierPointAtTime(time, &leftX, &leftY,true);
    bool isOnKeyframe = getRightBezierPointAtTime(time, &rightX, &rightY,true);
    double newLeftX = leftX,newLeftY = leftY,newRightX = rightX,newRightY = rightY;
    cuspTangent(x, y, &newLeftX, &newLeftY, pixelScale);
    cuspTangent(x, y, &newRightX, &newRightY, pixelScale);

    bool keyframeSet = false;

    if (autoKeying || isOnKeyframe) {
        setLeftBezierPointAtTime(time, newLeftX, newLeftY);
        setRightBezierPointAtTime(time, newRightX, newRightY);
        if (!isOnKeyframe) {
            keyframeSet = true;
        }
    }

    if (rippleEdit) {
        std::set<int> times;
        getKeyframeTimes(&times);
        for (std::set<int>::iterator it = times.begin(); it != times.end(); ++it) {
            setLeftBezierPointAtTime(*it, newLeftX, newLeftY);
            setRightBezierPointAtTime(*it, newRightX, newRightY);
        }
    }

    return keyframeSet;
}

bool
BezierCP::smoothPoint(int time,
                      bool autoKeying,
                      bool rippleEdit,
                      const std::pair<double,double>& pixelScale)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        ///update the offset time
        QWriteLocker l(&_imp->masterMutex);
        if (_imp->masterTrack) {
            _imp->offsetTime = time;
        }
    }

    double x,y,leftX,leftY,rightX,rightY;
    getPositionAtTime(time, &x, &y,true);
    getLeftBezierPointAtTime(time, &leftX, &leftY,true);
    bool isOnKeyframe = getRightBezierPointAtTime(time, &rightX, &rightY,true);

    smoothTangent(time,true,this,x, y, &leftX, &leftY, pixelScale);
    smoothTangent(time,false,this,x, y, &rightX, &rightY, pixelScale);

    bool keyframeSet = false;

    if (autoKeying || isOnKeyframe) {
        setLeftBezierPointAtTime(time, leftX, leftY);
        setRightBezierPointAtTime(time, rightX, rightY);
        if (!isOnKeyframe) {
            keyframeSet = true;
        }
    }

    if (rippleEdit) {
        std::set<int> times;
        getKeyframeTimes(&times);
        for (std::set<int>::iterator it = times.begin(); it != times.end(); ++it) {
            setLeftBezierPointAtTime(*it, leftX, leftY);
            setRightBezierPointAtTime(*it, rightX, rightY);
        }
    }

    return keyframeSet;
}

boost::shared_ptr<Curve>
BezierCP::getXCurve() const
{
    return _imp->curveX;
}

boost::shared_ptr<Curve>
BezierCP::getYCurve() const
{
    return _imp->curveY;
}

boost::shared_ptr<Curve>
BezierCP::getLeftXCurve() const
{
    return _imp->curveLeftBezierX;
}

boost::shared_ptr<Curve>
BezierCP::getLeftYCurve() const
{
    return _imp->curveLeftBezierY;
}

boost::shared_ptr<Curve>
BezierCP::getRightXCurve() const
{
    return _imp->curveRightBezierX;
}

boost::shared_ptr<Curve>
BezierCP::getRightYCurve() const
{
    return _imp->curveRightBezierY;
}

void
BezierCP::clone(const BezierCP & other)
{
    _imp->curveX->clone(*other._imp->curveX);
    _imp->curveY->clone(*other._imp->curveY);
    _imp->curveLeftBezierX->clone(*other._imp->curveLeftBezierX);
    _imp->curveLeftBezierY->clone(*other._imp->curveLeftBezierY);
    _imp->curveRightBezierX->clone(*other._imp->curveRightBezierX);
    _imp->curveRightBezierY->clone(*other._imp->curveRightBezierY);

    {
        QMutexLocker l(&_imp->staticPositionMutex);
        _imp->x = other._imp->x;
        _imp->y = other._imp->y;
        _imp->leftX = other._imp->leftX;
        _imp->leftY = other._imp->leftY;
        _imp->rightX = other._imp->rightX;
        _imp->rightY = other._imp->rightY;
    }

    {
        QWriteLocker l(&_imp->masterMutex);
        _imp->masterTrack = other._imp->masterTrack;
        _imp->offsetTime = other._imp->offsetTime;
    }
}

bool
BezierCP::equalsAtTime(int time,
                       const BezierCP & other) const
{
    double x,y,leftX,leftY,rightX,rightY;

    getPositionAtTime(time, &x, &y,true);
    getLeftBezierPointAtTime(time, &leftX, &leftY,true);
    getRightBezierPointAtTime(time, &rightX, &rightY,true);

    double ox,oy,oLeftX,oLeftY,oRightX,oRightY;
    other.getPositionAtTime(time, &ox, &oy,true);
    other.getLeftBezierPointAtTime(time, &oLeftX, &oLeftY,true);
    other.getRightBezierPointAtTime(time, &oRightX, &oRightY,true);

    if ( (x == ox) && (y == oy) && (leftX == oLeftX) && (leftY == oLeftY) && (rightX == oRightX) && (rightY == oRightY) ) {
        return true;
    }

    return false;
}

SequenceTime
BezierCP::getOffsetTime() const
{
    QReadLocker l(&_imp->masterMutex);

    return _imp->offsetTime;
}

void
BezierCP::slaveTo(SequenceTime offsetTime,
                  const boost::shared_ptr<Double_Knob> & track)
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->masterTrack);
    QWriteLocker l(&_imp->masterMutex);
    _imp->masterTrack = track;
    _imp->offsetTime = offsetTime;
}

void
BezierCP::unslave()
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->masterTrack);
    QWriteLocker l(&_imp->masterMutex);
    _imp->masterTrack.reset();
}

boost::shared_ptr<Double_Knob>
BezierCP::isSlaved() const
{
    QReadLocker l(&_imp->masterMutex);

    return _imp->masterTrack;
}

////////////////////////////////////RotoItem////////////////////////////////////
namespace {
class RotoMetaTypesRegistration
{
public:
    inline RotoMetaTypesRegistration()
    {
        qRegisterMetaType<RotoItem*>("RotoItem");
    }
};
}

static RotoMetaTypesRegistration registration;
RotoItem::RotoItem(const boost::shared_ptr<RotoContext>& context,
                   const std::string & name,
                   boost::shared_ptr<RotoLayer> parent)
    : itemMutex()
      , _imp( new RotoItemPrivate(context,name,parent) )
{
}

RotoItem::~RotoItem()
{
}

void
RotoItem::clone(const RotoItem*  other)
{
    QMutexLocker l(&itemMutex);

    _imp->parentLayer = other->_imp->parentLayer;
    _imp->scriptName = other->_imp->scriptName;
    _imp->label = other->_imp->label;
    _imp->globallyActivated = other->_imp->globallyActivated;
    _imp->locked = other->_imp->locked;
}

void
RotoItem::setParentLayer(boost::shared_ptr<RotoLayer> layer)
{
    ///called on the main-thread only
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&itemMutex);
    _imp->parentLayer = layer;
}

boost::shared_ptr<RotoLayer>
RotoItem::getParentLayer() const
{
    QMutexLocker l(&itemMutex);
    return _imp->parentLayer.lock();
}

void
RotoItem::setGloballyActivated_recursive(bool a)
{
    {
        QMutexLocker l(&itemMutex);
        _imp->globallyActivated = a;
        RotoLayer* layer = dynamic_cast<RotoLayer*>(this);
        if (layer) {
            const RotoItems & children = layer->getItems();
            for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
                (*it)->setGloballyActivated_recursive(a);
            }
        }
    }
}

void
RotoItem::setGloballyActivated(bool a,
                               bool setChildren)
{
    ///called on the main-thread only
    assert( QThread::currentThread() == qApp->thread() );
    if (setChildren) {
        setGloballyActivated_recursive(a);
    } else {
        QMutexLocker l(&itemMutex);
        _imp->globallyActivated = a;
    }
    boost::shared_ptr<RotoContext> c = _imp->context.lock();
    if (c) {
        c->evaluateChange();
    }
}

bool
RotoItem::isGloballyActivated() const
{
    QMutexLocker l(&itemMutex);

    return _imp->globallyActivated;
}

static bool
isDeactivated_imp(const boost::shared_ptr<RotoLayer>& item)
{
    if ( !item->isGloballyActivated() ) {
        return true;
    } else {
        boost::shared_ptr<RotoLayer> parent = item->getParentLayer();
        if (parent) {
            return isDeactivated_imp(parent);
        }
    }
    return false;
}

bool
RotoItem::isDeactivatedRecursive() const
{
    boost::shared_ptr<RotoLayer> parent;
    {
        QMutexLocker l(&itemMutex);
        if (!_imp->globallyActivated) {
            return true;
        }
        parent = _imp->parentLayer.lock();
    }

    if (parent) {
        return isDeactivated_imp(parent);
    }
    return false;
}

void
RotoItem::setLocked_recursive(bool locked,RotoItem::SelectionReasonEnum reason)
{
    {
        {
            QMutexLocker m(&itemMutex);
            _imp->locked = locked;
        }
        getContext()->onItemLockedChanged(shared_from_this(),reason);
        RotoLayer* layer = dynamic_cast<RotoLayer*>(this);
        if (layer) {
            const RotoItems & children = layer->getItems();
            for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
                (*it)->setLocked_recursive(locked,reason);
            }
        }
    }
}

void
RotoItem::setLocked(bool l,
                    bool lockChildren,
                    RotoItem::SelectionReasonEnum reason)
{
    ///called on the main-thread only
    assert( QThread::currentThread() == qApp->thread() );
    if (!lockChildren) {
        {
            QMutexLocker m(&itemMutex);
            _imp->locked = l;
        }
        getContext()->onItemLockedChanged(shared_from_this(),reason);
    } else {
        setLocked_recursive(l,reason);
    }
}

bool
RotoItem::getLocked() const
{
    QMutexLocker l(&itemMutex);

    return _imp->locked;
}

static
bool
isLocked_imp(const boost::shared_ptr<RotoLayer>& item)
{
    if ( item->getLocked() ) {
        return true;
    } else {
        boost::shared_ptr<RotoLayer> parent = item->getParentLayer();
        if (parent) {
            return isLocked_imp(parent);
        }
    }
    return false;
}

bool
RotoItem::isLockedRecursive() const
{
    boost::shared_ptr<RotoLayer> parent;
    {
        QMutexLocker l(&itemMutex);
        if (_imp->locked) {
            return true;
        }
        parent = _imp->parentLayer.lock();
    }

    if (parent) {
        return isLocked_imp(parent);
    } else {
        return false;
    }
}

int
RotoItem::getHierarchyLevel() const
{
    int ret = 0;
    boost::shared_ptr<RotoLayer> parent;

    {
        QMutexLocker l(&itemMutex);
        parent = _imp->parentLayer.lock();
    }

    while (parent) {
        parent = parent->getParentLayer();
        ++ret;
    }

    return ret;
}

boost::shared_ptr<RotoContext>
RotoItem::getContext() const
{
    return _imp->context.lock();
}

bool
RotoItem::setScriptName(const std::string & name)
{
    ///called on the main-thread only
    assert( QThread::currentThread() == qApp->thread() );
    
    if (name.empty()) {
        return false;
    }
    
    
    std::string cpy = Natron::makeNameScriptFriendly(name);
    
    if (cpy.empty()) {
        return false;
    }
    
    boost::shared_ptr<RotoItem> existingItem = getContext()->getItemByName(name);
    if ( existingItem && (existingItem.get() != this) ) {
        return false;
    }
    
    std::string oldFullName = getFullyQualifiedName();
    bool oldNameEmpty;
    {
        QMutexLocker l(&itemMutex);
        oldNameEmpty = _imp->scriptName.empty();
        _imp->scriptName = name;
    }
    std::string newFullName = getFullyQualifiedName();
    
    boost::shared_ptr<RotoContext> c = _imp->context.lock();
    if (c) {
        if (!oldNameEmpty) {
            c->changeItemScriptName(oldFullName, newFullName);
        }
        c->onItemScriptNameChanged(shared_from_this());
    }
    return true;
}

static void getScriptNameRecursive(RotoLayer* item,std::string* scriptName)
{
    scriptName->insert(0, ".");
    scriptName->insert(0, item->getScriptName());
    boost::shared_ptr<RotoLayer> parent = item->getParentLayer();
    if (parent) {
        getScriptNameRecursive(parent.get(), scriptName);
    }
}

std::string
RotoItem::getFullyQualifiedName() const
{
    std::string name = getScriptName();
    boost::shared_ptr<RotoLayer> parent = getParentLayer();
    if (parent) {
        getScriptNameRecursive(parent.get(), &name);
    }
    return name;
}

std::string
RotoItem::getScriptName() const
{
    QMutexLocker l(&itemMutex);

    return _imp->scriptName;
}

std::string
RotoItem::getLabel() const
{
    QMutexLocker l(&itemMutex);
    return _imp->label;
}

void
RotoItem::setLabel(const std::string& label)
{
    {
        QMutexLocker l(&itemMutex);
        _imp->label = label;
    }
    boost::shared_ptr<RotoContext> c = _imp->context.lock();
    if (c) {
        c->onItemLabelChanged(shared_from_this());
    }
}

void
RotoItem::save(RotoItemSerialization *obj) const
{
    boost::shared_ptr<RotoLayer> parent;
    {
        QMutexLocker l(&itemMutex);
        obj->activated = _imp->globallyActivated;
        obj->name = _imp->scriptName;
        obj->label = _imp->label;
        obj->locked = _imp->locked;
        parent = _imp->parentLayer.lock();
    }

    if (parent) {
        obj->parentLayerName = parent->getScriptName();
    }
}

void
RotoItem::load(const RotoItemSerialization &obj)
{
    {
        QMutexLocker l(&itemMutex);
        _imp->globallyActivated = obj.activated;
        _imp->locked = obj.locked;
        _imp->scriptName = obj.name;
        if (!obj.label.empty()) {
            _imp->label = obj.label;
        } else {
            _imp->label = _imp->scriptName;
        }
        std::locale loc;
        std::string cpy;
        for (std::size_t i = 0; i < _imp->scriptName.size(); ++i) {
            
            ///Ignore starting digits
            if (cpy.empty() && std::isdigit(_imp->scriptName[i],loc)) {
                continue;
            }
            
            ///Spaces becomes underscores
            if (std::isspace(_imp->scriptName[i],loc)){
                cpy.push_back('_');
            }
            
            ///Non alpha-numeric characters are not allowed in python
            else if (_imp->scriptName[i] == '_' || std::isalnum(_imp->scriptName[i], loc)) {
                cpy.push_back(_imp->scriptName[i]);
            }
        }
        if (!cpy.empty()) {
            _imp->scriptName = cpy;
        } else {
            l.unlock();
            std::string name = getContext()->generateUniqueName(kRotoBezierBaseName);
            l.relock();
            _imp->scriptName = name;
        }
    }
    boost::shared_ptr<RotoLayer> parent = getContext()->getLayerByName(obj.parentLayerName);

    {
        QMutexLocker l(&itemMutex);
        _imp->parentLayer = parent;
    }
}

std::string
RotoItem::getRotoNodeName() const
{
    return getContext()->getRotoNodeName();
}

////////////////////////////////////RotoDrawableItem////////////////////////////////////

RotoDrawableItem::RotoDrawableItem(const boost::shared_ptr<RotoContext>& context,
                                   const std::string & name,
                                   const boost::shared_ptr<RotoLayer>& parent)
    : RotoItem(context,name,parent)
      , _imp( new RotoDrawableItemPrivate() )
{
#ifdef NATRON_ROTO_INVERTIBLE
    QObject::connect( _imp->inverted->getSignalSlotHandler().get(), SIGNAL( valueChanged(int,int) ), this, SIGNAL( invertedStateChanged() ) );
#endif
    QObject::connect( this, SIGNAL( overlayColorChanged() ), context.get(), SIGNAL( refreshViewerOverlays() ) );
    QObject::connect( _imp->color->getSignalSlotHandler().get(), SIGNAL( valueChanged(int,int) ), this, SIGNAL( shapeColorChanged() ) );
    QObject::connect( _imp->compOperator->getSignalSlotHandler().get(), SIGNAL( valueChanged(int,int) ), this,
                      SIGNAL( compositingOperatorChanged(int,int) ) );
}

RotoDrawableItem::~RotoDrawableItem()
{
}

void
RotoDrawableItem::addKnob(const boost::shared_ptr<KnobI>& knob)
{
    _imp->knobs.push_back(knob);
}

void
RotoDrawableItem::clone(const RotoItem* other)
{
    const RotoDrawableItem* otherDrawable = dynamic_cast<const RotoDrawableItem*>(other);
    if (!otherDrawable) {
        return;
    }
    const std::list<boost::shared_ptr<KnobI> >& otherKnobs = otherDrawable->getKnobs();
    assert(otherKnobs.size() == _imp->knobs.size());
    if (otherKnobs.size() != _imp->knobs.size()) {
        return;
    }
    std::list<boost::shared_ptr<KnobI> >::iterator it = _imp->knobs.begin();
    std::list<boost::shared_ptr<KnobI> >::const_iterator otherIt = otherKnobs.begin();
    for (; it != _imp->knobs.end(); ++it, ++otherIt) {
        (*it)->clone(*otherIt);
    }
    {
        QMutexLocker l(&itemMutex);
        memcpy(_imp->overlayColor, otherDrawable->_imp->overlayColor, sizeof(double) * 4);
    }
    RotoItem::clone(other);
}

static void
serializeRotoKnob(const boost::shared_ptr<KnobI> & knob,
                  KnobSerialization* serialization)
{
    std::pair<int, boost::shared_ptr<KnobI> > master = knob->getMaster(0);
    bool wasSlaved = false;

    if (master.second) {
        wasSlaved = true;
        knob->unSlave(0,false);
    }

    serialization->initialize(knob);

    if (wasSlaved) {
        knob->slaveTo(0, master.second, master.first);
    }
}

void
RotoDrawableItem::save(RotoItemSerialization *obj) const
{
    RotoDrawableItemSerialization* s = dynamic_cast<RotoDrawableItemSerialization*>(obj);

    assert(s);

    {
        serializeRotoKnob(_imp->activated, &s->_activated);
        serializeRotoKnob(_imp->feather, &s->_feather);
        serializeRotoKnob(_imp->opacity, &s->_opacity);
        serializeRotoKnob(_imp->featherFallOff, &s->_featherFallOff);
#ifdef NATRON_ROTO_INVERTIBLE
        serializeRotoKnob(_imp->inverted, &s->_inverted);
#endif
        serializeRotoKnob(_imp->color, &s->_color);
        serializeRotoKnob(_imp->compOperator, &s->_compOp);
        
        QMutexLocker l(&itemMutex);
        memcpy(s->_overlayColor, _imp->overlayColor, sizeof(double) * 4);
    }
    RotoItem::save(obj);
}

void
RotoDrawableItem::load(const RotoItemSerialization &obj)
{
    const RotoDrawableItemSerialization & s = dynamic_cast<const RotoDrawableItemSerialization &>(obj);


    {
        _imp->activated->clone( s._activated.getKnob().get() );
        _imp->opacity->clone( s._opacity.getKnob().get() );
        _imp->feather->clone( s._feather.getKnob().get() );
        _imp->featherFallOff->clone( s._featherFallOff.getKnob().get() );
#ifdef NATRON_ROTO_INVERTIBLE
        _imp->inverted->clone( s._inverted.getKnob().get() );
#endif
        if (s._hasColorAndCompOp) {
            _imp->color->clone( s._color.getKnob().get() );
            _imp->compOperator->clone( s._compOp.getKnob().get() );
        }
        QMutexLocker l(&itemMutex);
        memcpy(_imp->overlayColor, s._overlayColor, sizeof(double) * 4);
    }
    RotoItem::load(obj);
}

bool
RotoDrawableItem::isActivated(int time) const
{
//    bool deactivated = isDeactivatedRecursive();
//    if (deactivated) {
//        return false;
//    } else {
    return _imp->activated->getValueAtTime(time);
//    }
}

void
RotoDrawableItem::setActivated(bool a, int time)
{
    _imp->activated->setValueAtTime(time, a, 0);
    getContext()->onItemKnobChanged();
}

double
RotoDrawableItem::getOpacity(int time) const
{
    ///MT-safe thanks to Knob
    return _imp->opacity->getValueAtTime(time);
}

void
RotoDrawableItem::setOpacity(double o,int time)
{
    _imp->opacity->setValueAtTime(time, o, 0);
    getContext()->onItemKnobChanged();
}

double
RotoDrawableItem::getFeatherDistance(int time) const
{
    ///MT-safe thanks to Knob
    return _imp->feather->getValueAtTime(time);
}

void
RotoDrawableItem::setFeatherDistance(double d,int time)
{
    _imp->feather->setValueAtTime(time, d, 0);
    getContext()->onItemKnobChanged();
}


int
RotoDrawableItem::getNumKeyframesFeatherDistance() const
{
    return _imp->feather->getKeyFramesCount(0);
}

void
RotoDrawableItem::setFeatherFallOff(double f,int time)
{
    _imp->featherFallOff->setValueAtTime(time, f, 0);
    getContext()->onItemKnobChanged();
}

double
RotoDrawableItem::getFeatherFallOff(int time) const
{
    ///MT-safe thanks to Knob
    return _imp->featherFallOff->getValueAtTime(time);
}

#ifdef NATRON_ROTO_INVERTIBLE
bool
RotoDrawableItem::getInverted(int time) const
{
    ///MT-safe thanks to Knob
    return _imp->inverted->getValueAtTime(time);
}

#endif

void
RotoDrawableItem::getColor(int time,
                           double* color) const
{
    color[0] = _imp->color->getValueAtTime(time,0);
    color[1] = _imp->color->getValueAtTime(time,1);
    color[2] = _imp->color->getValueAtTime(time,2);
}

void
RotoDrawableItem::setColor(int time,double r,double g,double b)
{
    _imp->color->setValueAtTime(time, r, 0);
    _imp->color->setValueAtTime(time, g, 1);
    _imp->color->setValueAtTime(time, b, 2);
    getContext()->onItemKnobChanged();
}

int
RotoDrawableItem::getCompositingOperator() const
{
    return _imp->compOperator->getValue();
}

void
RotoDrawableItem::setCompositingOperator(int op)
{
    _imp->compOperator->setValue( op, 0);
}

std::string
RotoDrawableItem::getCompositingOperatorToolTip() const
{
    return _imp->compOperator->getHintToolTipFull();
}

void
RotoDrawableItem::getOverlayColor(double* color) const
{
    QMutexLocker l(&itemMutex);

    memcpy(color, _imp->overlayColor, sizeof(double) * 4);
}

void
RotoDrawableItem::setOverlayColor(const double *color)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker l(&itemMutex);
        memcpy(_imp->overlayColor, color, sizeof(double) * 4);
    }
    Q_EMIT overlayColorChanged();
}

boost::shared_ptr<Bool_Knob> RotoDrawableItem::getActivatedKnob() const
{
    return _imp->activated;
}

boost::shared_ptr<Double_Knob> RotoDrawableItem::getFeatherKnob() const
{
    return _imp->feather;
}

boost::shared_ptr<Double_Knob> RotoDrawableItem::getFeatherFallOffKnob() const
{
    return _imp->featherFallOff;
}

boost::shared_ptr<Double_Knob> RotoDrawableItem::getOpacityKnob() const
{
    return _imp->opacity;
}

#ifdef NATRON_ROTO_INVERTIBLE
boost::shared_ptr<Bool_Knob> RotoDrawableItem::getInvertedKnob() const
{
    return _imp->inverted;
}

#endif
boost::shared_ptr<Choice_Knob> RotoDrawableItem::getOperatorKnob() const
{
    return _imp->compOperator;
}

boost::shared_ptr<Color_Knob> RotoDrawableItem::getColorKnob() const
{
    return _imp->color;
}

const std::list<boost::shared_ptr<KnobI> >&
RotoDrawableItem::getKnobs() const
{
    return _imp->knobs;
}




////////////////////////////////////Layer////////////////////////////////////

RotoLayer::RotoLayer(const boost::shared_ptr<RotoContext>& context,
                     const std::string & n,
                     const boost::shared_ptr<RotoLayer>& parent)
    : RotoItem(context,n,parent)
      , _imp( new RotoLayerPrivate() )
{
}

RotoLayer::RotoLayer(const RotoLayer & other)
    : RotoItem( other.getContext(),other.getScriptName(),other.getParentLayer() )
      ,_imp( new RotoLayerPrivate() )
{
    clone(&other);
}

RotoLayer::~RotoLayer()
{
}

#if 0
void
RotoLayer::clone(const RotoLayer & other)
{
    RotoItem::clone(other);
    QMutexLocker l(&itemMutex);

    _imp->items.clear();
    for (std::list<boost::shared_ptr<RotoItem> >::const_iterator it = other._imp->items.begin(); it != other._imp->items.end(); ++it) {
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(it);
        if (isBezier) {
            boost::shared_ptr<Bezier> copy( new Bezier(*isBezier, this) );
            copy->setParentLayer(this);
            _imp->items.push_back(copy);
        } else {
            assert(isLayer);
            if (isLayer) {
                boost::shared_ptr<RotoLayer> copy( new RotoLayer(*isLayer) );
                copy->setParentLayer(this);
                _imp->items.push_back(copy);
                getContext()->addLayer(copy);
            }
        }
    }
}
#endif

void
RotoLayer::save(RotoItemSerialization *obj) const
{
    RotoLayerSerialization* s = dynamic_cast<RotoLayerSerialization*>(obj);

    assert(s);
    RotoItems items;
    {
        QMutexLocker l(&itemMutex);
        items = _imp->items;
    }

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        boost::shared_ptr<RotoStrokeItem> isStroke = boost::dynamic_pointer_cast<RotoStrokeItem>(*it);
        boost::shared_ptr<RotoLayer> layer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<RotoItemSerialization> childSerialization;
        if (isBezier && !isStroke) {
            childSerialization.reset(new BezierSerialization);
            isBezier->save( childSerialization.get() );
        } else if (isStroke) {
            childSerialization.reset(new RotoStrokeSerialization);
            isStroke->save(childSerialization.get());
        } else {
            assert(layer);
            if (layer) {
                childSerialization.reset(new RotoLayerSerialization);
                layer->save( childSerialization.get() );
            }
        }
        assert(childSerialization);
        s->children.push_back(childSerialization);
    }


    RotoItem::save(obj);
}

void
RotoLayer::load(const RotoItemSerialization &obj)
{
    const RotoLayerSerialization & s = dynamic_cast<const RotoLayerSerialization &>(obj);
    boost::shared_ptr<RotoLayer> this_layer = boost::dynamic_pointer_cast<RotoLayer>(shared_from_this());
    assert(this_layer);
    RotoItem::load(obj);
    {
        for (std::list<boost::shared_ptr<RotoItemSerialization> >::const_iterator it = s.children.begin(); it != s.children.end(); ++it) {
            boost::shared_ptr<BezierSerialization> b = boost::dynamic_pointer_cast<BezierSerialization>(*it);
            boost::shared_ptr<RotoStrokeSerialization> s = boost::dynamic_pointer_cast<RotoStrokeSerialization>(*it);
            boost::shared_ptr<RotoLayerSerialization> l = boost::dynamic_pointer_cast<RotoLayerSerialization>(*it);
            if (b && !s) {
                boost::shared_ptr<Bezier> bezier( new Bezier(getContext(), kRotoBezierBaseName, this_layer) );
                bezier->load(*b);

                QMutexLocker l(&itemMutex);
                _imp->items.push_back(bezier);
            }
#ifdef ROTO_ENABLE_PAINT
            else if (s) {
                boost::shared_ptr<RotoStrokeItem> stroke(new RotoStrokeItem((Natron::RotoStrokeType)s->getType(),getContext(),kRotoPaintBrushBaseName,this_layer));
                stroke->load(*s);
                
                QMutexLocker l(&itemMutex);
                _imp->items.push_back(stroke);
            }
#endif
            else if (l) {
                boost::shared_ptr<RotoLayer> layer( new RotoLayer(getContext(), kRotoLayerBaseName, this_layer) );
                _imp->items.push_back(layer);
                getContext()->addLayer(layer);
                layer->load(*l);
            }
        }
    }
}

void
RotoLayer::addItem(const boost::shared_ptr<RotoItem> & item,bool declareToPython )
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    boost::shared_ptr<RotoLayer> parentLayer = item->getParentLayer();
    if (parentLayer) {
        parentLayer->removeItem(item);
    }
    
    item->setParentLayer(boost::dynamic_pointer_cast<RotoLayer>(shared_from_this()));
    {
        QMutexLocker l(&itemMutex);
        _imp->items.push_back(item);
    }
    if (declareToPython) {
        getContext()->declareItemAsPythonField(item);
    }
}

void
RotoLayer::insertItem(const boost::shared_ptr<RotoItem> & item,
                      int index)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(index >= 0);
    
    boost::shared_ptr<RotoLayer> parentLayer = item->getParentLayer();
    if (parentLayer) {
        parentLayer->removeItem(item);
    }
    
    item->setParentLayer(boost::dynamic_pointer_cast<RotoLayer>(shared_from_this()));
    {
        QMutexLocker l(&itemMutex);
        RotoItems::iterator it = _imp->items.begin();
        if ( index >= (int)_imp->items.size() ) {
            it = _imp->items.end();
        } else {
            std::advance(it, index);
        }
        ///insert before the iterator
        _imp->items.insert(it, item);
    }
    getContext()->declareItemAsPythonField(item);
}

void
RotoLayer::removeItem(const boost::shared_ptr<RotoItem>& item)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker l(&itemMutex);
        for (RotoItems::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it) {
            if (*it == item) {
                l.unlock();
                getContext()->removeItemAsPythonField(item);
                l.relock();
                item->setParentLayer(boost::shared_ptr<RotoLayer>());
                _imp->items.erase(it);
                break;
            }
        }
    }
}

int
RotoLayer::getChildIndex(const boost::shared_ptr<RotoItem> & item) const
{
    QMutexLocker l(&itemMutex);
    int i = 0;

    for (RotoItems::iterator it = _imp->items.begin(); it != _imp->items.end(); ++it, ++i) {
        if (*it == item) {
            return i;
        }
    }

    return -1;
}

const RotoItems &
RotoLayer::getItems() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->items;
}

RotoItems
RotoLayer::getItems_mt_safe() const
{
    QMutexLocker l(&itemMutex);

    return _imp->items;
}

////////////////////////////////////Bezier////////////////////////////////////

namespace  {
enum SplineChangedReason
{
    DERIVATIVES_CHANGED = 0,
    CONTROL_POINT_CHANGED = 1
};
}


Bezier::Bezier(const boost::shared_ptr<RotoContext>& ctx,
               const std::string & name,
               const boost::shared_ptr<RotoLayer>& parent)
    : RotoDrawableItem(ctx,name,parent)
      , _imp( new BezierPrivate() )
{
}


Bezier::Bezier(const Bezier & other,
               const boost::shared_ptr<RotoLayer>& parent)
: RotoDrawableItem( other.getContext(), other.getScriptName(), other.getParentLayer() )
, _imp( new BezierPrivate() )
{
    clone(&other);
    setParentLayer(parent);
}

void
Bezier::clearAllPoints()
{
    removeAnimation();
    QMutexLocker k(&itemMutex);
    _imp->points.clear();
    _imp->featherPoints.clear();
    _imp->isClockwiseOriented.clear();
    _imp->finished = false;
}

void
Bezier::clone(const RotoItem* other)
{
    boost::shared_ptr<Bezier> this_shared = boost::dynamic_pointer_cast<Bezier>(shared_from_this());
    assert(this_shared);
    
    const Bezier* otherBezier = dynamic_cast<const Bezier*>(other);
    if (!otherBezier) {
        return;
    }
    
    Q_EMIT aboutToClone();
    {
        bool useFeather = otherBezier->useFeatherPoints();
        QMutexLocker l(&itemMutex);
        assert(otherBezier->_imp->featherPoints.size() == otherBezier->_imp->points.size() || !useFeather);

        _imp->featherPoints.clear();
        _imp->points.clear();
        BezierCPs::const_iterator itF = otherBezier->_imp->featherPoints.begin();
        for (BezierCPs::const_iterator it = otherBezier->_imp->points.begin(); it != otherBezier->_imp->points.end(); ++it) {
            boost::shared_ptr<BezierCP> cp( new BezierCP(this_shared) );
            cp->clone(**it);
            _imp->points.push_back(cp);
            if (useFeather) {
                boost::shared_ptr<BezierCP> fp( new BezierCP(this_shared) );
                fp->clone(**itF);
                _imp->featherPoints.push_back(fp);
                ++itF;
            }
        }
        _imp->finished = otherBezier->_imp->finished;
    }
    RotoDrawableItem::clone(other);
    Q_EMIT cloned();
}

Bezier::~Bezier()
{
    BezierCPs::iterator itFp = _imp->featherPoints.begin();
    bool useFeather = !_imp->featherPoints.empty();
    for (BezierCPs::iterator itCp = _imp->points.begin(); itCp != _imp->points.end(); ++itCp) {
        boost::shared_ptr<Double_Knob> masterCp = (*itCp)->isSlaved();
        
        if (masterCp) {
            masterCp->removeSlavedTrack(*itCp);
        }
        
        if (useFeather) {
            boost::shared_ptr<Double_Knob> masterFp = (*itFp)->isSlaved();
            if (masterFp) {
                masterFp->removeSlavedTrack(*itFp);
            }
            ++itFp;
        }
    }
}

boost::shared_ptr<BezierCP>
Bezier::addControlPoint(double x,
                        double y,
                        int time)
{
    
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    if (isCurveFinished()) {
        return boost::shared_ptr<BezierCP>();
    }
    
    int keyframeTime;
    ///if the curve is empty make a new keyframe at the current timeline's time
    ///otherwise re-use the time at which the keyframe was set on the first control point
    if ( _imp->points.empty() ) {
        keyframeTime = time;
    } else {
        keyframeTime = _imp->points.front()->getKeyframeTime(0);
        if (keyframeTime == INT_MAX) {
            keyframeTime = getContext()->getTimelineCurrentTime();
        }
    }
    
    boost::shared_ptr<BezierCP> p;
    boost::shared_ptr<Bezier> this_shared = boost::dynamic_pointer_cast<Bezier>(shared_from_this());
    assert(this_shared);
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    {
        QMutexLocker l(&itemMutex);
        assert(!_imp->finished);

    
        p.reset( new BezierCP(this_shared) );
        if (autoKeying) {
            p->setPositionAtTime(keyframeTime, x, y);
            p->setLeftBezierPointAtTime(keyframeTime, x,y);
            p->setRightBezierPointAtTime(keyframeTime, x, y);
        } else {
            p->setStaticPosition(x, y);
            p->setLeftBezierStaticPosition(x, y);
            p->setRightBezierStaticPosition(x, y);
        }
        _imp->points.insert(_imp->points.end(),p);
        
        if (useFeatherPoints()) {
            boost::shared_ptr<BezierCP> fp( new FeatherPoint(this_shared) );
            if (autoKeying) {
                fp->setPositionAtTime(keyframeTime, x, y);
                fp->setLeftBezierPointAtTime(keyframeTime, x, y);
                fp->setRightBezierPointAtTime(keyframeTime, x, y);
            } else {
                fp->setStaticPosition(x, y);
                fp->setLeftBezierStaticPosition(x, y);
                fp->setRightBezierStaticPosition(x, y);
            }
            _imp->featherPoints.insert(_imp->featherPoints.end(),fp);
        }
    }
    
    return p;
}

boost::shared_ptr<BezierCP>
Bezier::addControlPointAfterIndex(int index,
                                  double t)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    boost::shared_ptr<Bezier> this_shared = boost::dynamic_pointer_cast<Bezier>(shared_from_this());
    assert(this_shared);

    boost::shared_ptr<BezierCP> p( new BezierCP(this_shared) );
    boost::shared_ptr<BezierCP> fp;
    
    if (useFeatherPoints()) {
        fp.reset( new FeatherPoint(this_shared) );
    }
    {
        QMutexLocker l(&itemMutex);
        
        if ( ( index >= (int)_imp->points.size() ) || (index < -1) ) {
            throw std::invalid_argument("Spline control point index out of range.");
        }
        
        
        ///we set the new control point position to be in the exact position the curve would have at each keyframe
        std::set<int> existingKeyframes;
        _imp->getKeyframeTimes(&existingKeyframes);
        
        BezierCPs::const_iterator prev,next,prevF,nextF;
        if (index == -1) {
            prev = _imp->points.end();
            if (prev != _imp->points.begin()) {
                --prev;
            }
            next = _imp->points.begin();
            
            if (useFeatherPoints()) {
                prevF = _imp->featherPoints.end();
                if (prevF != _imp->featherPoints.begin()) {
                    --prevF;
                }
                nextF = _imp->featherPoints.begin();
            }
        } else {
            prev = _imp->atIndex(index);
            next = prev;
            if (next != _imp->points.end()) {
                ++next;
            }
            if ( _imp->finished && ( next == _imp->points.end() ) ) {
                next = _imp->points.begin();
            }
            assert( next != _imp->points.end() );
            
            if (useFeatherPoints()) {
                prevF = _imp->featherPoints.begin();
                std::advance(prevF, index);
                nextF = prevF;
                if (nextF != _imp->featherPoints.end()) {
                    ++nextF;
                }
                if ( _imp->finished && ( nextF == _imp->featherPoints.end() ) ) {
                    nextF = _imp->featherPoints.begin();
                }
            }
        }
        
        
        for (std::set<int>::iterator it = existingKeyframes.begin(); it != existingKeyframes.end(); ++it) {
            Point p0,p1,p2,p3;
            (*prev)->getPositionAtTime(*it, &p0.x, &p0.y);
            (*prev)->getRightBezierPointAtTime(*it, &p1.x, &p1.y);
            (*next)->getPositionAtTime(*it, &p3.x, &p3.y);
            (*next)->getLeftBezierPointAtTime(*it, &p2.x, &p2.y);
            
            
            Point dest;
            Point p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3;
            bezierFullPoint(p0, p1, p2, p3, t, &p0p1, &p1p2, &p2p3, &p0p1_p1p2, &p1p2_p2p3, &dest);
            
            //update prev and next inner control points
            (*prev)->setRightBezierPointAtTime(*it, p0p1.x, p0p1.y);
            (*next)->setLeftBezierPointAtTime(*it, p2p3.x, p2p3.y);
            
            if (useFeatherPoints()) {
                (*prevF)->setRightBezierPointAtTime(*it, p0p1.x, p0p1.y);
                (*nextF)->setLeftBezierPointAtTime(*it, p2p3.x, p2p3.y);
            }
            
            
            p->setPositionAtTime(*it, dest.x, dest.y);
            ///The left control point of p is p0p1_p1p2 and the right control point is p1p2_p2p3
            p->setLeftBezierPointAtTime(*it, p0p1_p1p2.x, p0p1_p1p2.y);
            p->setRightBezierPointAtTime(*it, p1p2_p2p3.x, p1p2_p2p3.y);
            
            if (useFeatherPoints()) {
                fp->setPositionAtTime(*it, dest.x, dest.y);
                fp->setLeftBezierPointAtTime(*it, p0p1_p1p2.x, p0p1_p1p2.y);
                fp->setRightBezierPointAtTime(*it, p1p2_p2p3.x, p1p2_p2p3.y);
            }
        }
        
        ///if there's no keyframes
        if ( existingKeyframes.empty() ) {
            Point p0,p1,p2,p3;
            
            (*prev)->getPositionAtTime(0, &p0.x, &p0.y);
            (*prev)->getRightBezierPointAtTime(0, &p1.x, &p1.y);
            (*next)->getPositionAtTime(0, &p3.x, &p3.y);
            (*next)->getLeftBezierPointAtTime(0, &p2.x, &p2.y);
            
            
            Point dest;
            Point p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3;
            bezierFullPoint(p0, p1, p2, p3, t, &p0p1, &p1p2, &p2p3, &p0p1_p1p2, &p1p2_p2p3, &dest);
            
            //update prev and next inner control points
            (*prev)->setRightBezierStaticPosition(p0p1.x, p0p1.y);
            (*next)->setLeftBezierStaticPosition(p2p3.x, p2p3.y);
            
             if (useFeatherPoints()) {
                 (*prevF)->setRightBezierStaticPosition(p0p1.x, p0p1.y);
                 (*nextF)->setLeftBezierStaticPosition(p2p3.x, p2p3.y);
             }
            
            p->setStaticPosition(dest.x, dest.y);
            ///The left control point of p is p0p1_p1p2 and the right control point is p1p2_p2p3
            p->setLeftBezierStaticPosition(p0p1_p1p2.x, p0p1_p1p2.y);
            p->setRightBezierStaticPosition(p1p2_p2p3.x, p1p2_p2p3.y);
            
            if (useFeatherPoints()) {
                fp->setStaticPosition(dest.x, dest.y);
                fp->setLeftBezierStaticPosition(p0p1_p1p2.x, p0p1_p1p2.y);
                fp->setRightBezierStaticPosition(p1p2_p2p3.x, p1p2_p2p3.y);
            }
        }
        
        
        ////Insert the point into the container
        if (index != -1) {
            BezierCPs::iterator it = _imp->points.begin();
            ///it will point at the element right after index
            std::advance(it, index + 1);
            _imp->points.insert(it,p);
            
            if (useFeatherPoints()) {
                ///insert the feather point
                BezierCPs::iterator itF = _imp->featherPoints.begin();
                std::advance(itF, index + 1);
                _imp->featherPoints.insert(itF, fp);
            }
        } else {
            _imp->points.push_front(p);
             if (useFeatherPoints()) {
                 _imp->featherPoints.push_front(fp);
             }
        }
        
        
        ///If auto-keying is enabled, set a new keyframe
        int currentTime = getContext()->getTimelineCurrentTime();
        if ( !_imp->hasKeyframeAtTime(currentTime) && getContext()->isAutoKeyingEnabled() ) {
            l.unlock();
            setKeyframe(currentTime);
        }
  
    }
    return p;
} // addControlPointAfterIndex

int
Bezier::getControlPointsCount() const
{
    QMutexLocker l(&itemMutex);

    return (int)_imp->points.size();
}

int
Bezier::isPointOnCurve(double x,
                       double y,
                       double distance,
                       double *t,
                       bool* feather) const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    int time = getContext()->getTimelineCurrentTime();
    QMutexLocker l(&itemMutex);

    ///special case: if the curve has only 1 control point, just check if the point
    ///is nearby that sole control point
    if (_imp->points.size() == 1) {
        const boost::shared_ptr<BezierCP> & cp = _imp->points.front();
        if ( isPointCloseTo(time, *cp, x, y, distance) ) {
            *feather = false;
            
            return 0;
        } else {
            
            if (useFeatherPoints()) {
                ///do the same with the feather points
                const boost::shared_ptr<BezierCP> & fp = _imp->featherPoints.front();
                if ( isPointCloseTo(time, *fp, x, y, distance) ) {
                    *feather = true;
                    
                    return 0;
                }
            }
        }
        
        return -1;
    }

    ///For each segment find out if the point lies on the bezier
    int index = 0;

    bool useFeather = useFeatherPoints();
    
    assert( _imp->featherPoints.size() == _imp->points.size() || !useFeather);

    BezierCPs::const_iterator fp = _imp->featherPoints.begin();
    for (BezierCPs::const_iterator it = _imp->points.begin(); it != _imp->points.end(); ++it, ++index) {
        BezierCPs::const_iterator next = it;
        BezierCPs::const_iterator nextFp = fp;
        
        if (useFeather && nextFp != _imp->featherPoints.end()) {
            ++nextFp;
        }
        if (next != _imp->points.end()) {
            ++next;
        }
        if ( next == _imp->points.end() ) {
            if (!_imp->finished) {
                return -1;
            } else {
                next = _imp->points.begin();
                if (useFeather) {
                    nextFp = _imp->featherPoints.begin();
                }
            }
        }
        if ( bezierSegmentMeetsPoint(*(*it), *(*next), time, x, y, distance, t) ) {
            *feather = false;

            return index;
        }
        
        if (useFeather && bezierSegmentMeetsPoint(**fp, **nextFp, time, x, y, distance, t) ) {
            *feather = true;

            return index;
        }
        if (useFeather) {
            ++fp;
        }
    }

    ///now check the feather points segments only if they are different from the base bezier


    return -1;
} // isPointOnCurve

void
Bezier::setCurveFinished(bool finished)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker l(&itemMutex);
        _imp->finished = finished;
    }
    refreshPolygonOrientation();
}

bool
Bezier::isCurveFinished() const
{
    QMutexLocker l(&itemMutex);

    return _imp->finished;
}

void
Bezier::removeControlPointByIndex(int index)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    {
        QMutexLocker l(&itemMutex);
        BezierCPs::iterator it;
        try {
            it = _imp->atIndex(index);
        } catch (...) {
            ///attempt to remove an unexsiting point
            return;
        }
        
        boost::shared_ptr<Double_Knob> isSlaved = (*it)->isSlaved();
        if (isSlaved) {
            isSlaved->removeSlavedTrack(*it);
        }
        _imp->points.erase(it);
        
        if (useFeatherPoints()) {
            BezierCPs::iterator itF = _imp->featherPoints.begin();
            std::advance(itF, index);
            _imp->featherPoints.erase(itF);
            
        }
    }
    refreshPolygonOrientation();
    Q_EMIT controlPointRemoved();
}

void
Bezier::movePointByIndexInternal(int index,
                                 int time,
                                 double dx,
                                 double dy,
                                 bool onlyFeather)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    bool keySet = false;
    {
        QMutexLocker l(&itemMutex);
        double x,y,leftX,leftY,rightX,rightY;
        boost::shared_ptr<BezierCP> cp;
        bool isOnKeyframe = false;
        if (!onlyFeather) {
            BezierCPs::iterator it = _imp->atIndex(index);
            assert(it != _imp->points.end());
            cp = *it;
            cp->getPositionAtTime(time, &x, &y,true);
            isOnKeyframe |= cp->getLeftBezierPointAtTime(time, &leftX, &leftY,true);
            cp->getRightBezierPointAtTime(time, &rightX, &rightY,true);
        }
        
        bool useFeather = useFeatherPoints();
        double xF, yF, leftXF, leftYF, rightXF, rightYF;
        boost::shared_ptr<BezierCP> fp;
        if (useFeather) {
            BezierCPs::iterator itF = _imp->featherPoints.begin();
            std::advance(itF, index);
            assert(itF != _imp->featherPoints.end());
            fp = *itF;
            fp->getPositionAtTime(time, &xF, &yF,true);
            isOnKeyframe |= fp->getLeftBezierPointAtTime(time, &leftXF, &leftYF,true);
            fp->getRightBezierPointAtTime(time, &rightXF, &rightYF,true);
        }
        
        bool fLinkEnabled = (onlyFeather ? true : getContext()->isFeatherLinkEnabled());
        bool moveFeather = (fLinkEnabled || (useFeather && fp && cp->equalsAtTime(time, *fp)));
        
        
        if (!onlyFeather && (autoKeying || isOnKeyframe)) {
            assert(cp);
            cp->setPositionAtTime(time, x + dx, y + dy);
            cp->setLeftBezierPointAtTime(time, leftX + dx, leftY + dy);
            cp->setRightBezierPointAtTime(time, rightX + dx, rightY + dy);
            if (!isOnKeyframe) {
                keySet = true;
            }
        }
        
        if (moveFeather && useFeather) {
            if (autoKeying || isOnKeyframe) {
                assert(fp);
                fp->setPositionAtTime(time, xF + dx, yF + dy);
                fp->setLeftBezierPointAtTime(time, leftXF + dx, leftYF + dy);
                fp->setRightBezierPointAtTime(time, rightXF + dx, rightYF + dy);
            }
        }
        
        if ( getContext()->isRippleEditEnabled() ) {
            std::set<int> keyframes;
            _imp->getKeyframeTimes(&keyframes);
            for (std::set<int>::iterator it2 = keyframes.begin(); it2 != keyframes.end(); ++it2) {
                if (*it2 == time) {
                    continue;
                }
                if (!onlyFeather) {
                    assert(cp);
                    cp->getPositionAtTime(*it2, &x, &y,true);
                    cp->getLeftBezierPointAtTime(*it2, &leftX, &leftY,true);
                    cp->getRightBezierPointAtTime(*it2, &rightX, &rightY,true);
                    
                    cp->setPositionAtTime(*it2, x + dx, y + dy);
                    cp->setLeftBezierPointAtTime(*it2, leftX + dx, leftY + dy);
                    cp->setRightBezierPointAtTime(*it2, rightX + dx, rightY + dy);
                }
                if (moveFeather && useFeather) {
                    assert(fp);
                    fp->getPositionAtTime(*it2, &xF, &yF,true);
                    fp->getLeftBezierPointAtTime(*it2, &leftXF, &leftYF,true);
                    fp->getRightBezierPointAtTime(*it2, &rightXF, &rightYF,true);
                    
                    fp->setPositionAtTime(*it2, xF + dx, yF + dy);
                    fp->setLeftBezierPointAtTime(*it2, leftXF + dx, leftYF + dy);
                    fp->setRightBezierPointAtTime(*it2, rightXF + dx, rightYF + dy);
                }
            }
        }
    }
    refreshPolygonOrientation(time);
    if (autoKeying) {
        setKeyframe(time);
    }
    if (keySet) {
        Q_EMIT keyframeSet(time);
    }

} // movePointByIndexInternal

void
Bezier::movePointByIndex(int index,
                         int time,
                         double dx,
                         double dy)
{
    movePointByIndexInternal(index, time, dx, dy, false);
} // movePointByIndex

void
Bezier::moveFeatherByIndex(int index,
                           int time,
                           double dx,
                           double dy)
{
    movePointByIndexInternal(index, time, dx, dy, true);
} // moveFeatherByIndex

void
Bezier::moveBezierPointInternal(BezierCP* cpParam,
                                int index,
                                int time,
                                double lx, double ly,
                                double rx, double ry,
                                bool isLeft,
                                bool moveBoth)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    bool featherLink = getContext()->isFeatherLinkEnabled();
    bool rippleEdit = getContext()->isRippleEditEnabled();
    bool keySet = false;
    {
        QMutexLocker l(&itemMutex);
        BezierCP* cp = 0;
        BezierCP* fp = 0;
        
        if (!cpParam) {
            BezierCPs::iterator cpIt = _imp->atIndex(index);
            assert( cpIt != _imp->points.end() );
            cp = cpIt->get();
            assert(cp);
            
            if (useFeatherPoints()) {
                BezierCPs::iterator fpIt = _imp->featherPoints.begin();
                std::advance(fpIt, index);
                assert(fpIt != _imp->featherPoints.end());
                fp = fpIt->get();
                assert(fp);
            }
        } else {
            cp = cpParam;
        }
        
        bool isOnKeyframe;
        double leftX, leftY;
        if (isLeft || moveBoth) {
            isOnKeyframe = (cp)->getLeftBezierPointAtTime(time, &leftX, &leftY,true);
        }
        double rightX, rightY;
        if (!isLeft || moveBoth) {
            isOnKeyframe = (cp)->getRightBezierPointAtTime(time, &rightX, &rightY,true);
        }
        
        bool moveFeather = false;
        double leftXF, leftYF, rightXF, rightYF;
        if (!cpParam && useFeatherPoints()) {
            moveFeather = true;
            if (isLeft || moveBoth) {
                (fp)->getLeftBezierPointAtTime(time, &leftXF, &leftYF,true);
                moveFeather = moveFeather && leftX == leftXF && leftY == leftYF;
            }
            if (!isLeft || moveBoth) {
                (fp)->getRightBezierPointAtTime(time, &rightXF, &rightYF,true);
                moveFeather = moveFeather && rightX == rightXF && rightY == rightYF;
            }
            moveFeather = moveFeather || featherLink;
        }

        if (autoKeying || isOnKeyframe) {
            if (isLeft || moveBoth) {
                (cp)->setLeftBezierPointAtTime(time, leftX + lx, leftY + ly);
            }
            if (!isLeft || moveBoth) {
                (cp)->setRightBezierPointAtTime(time, rightX + rx, rightY + ry);
            }
            if (moveFeather && useFeatherPoints()) {
                if (isLeft || moveBoth) {
                    (fp)->setLeftBezierPointAtTime(time, leftXF + lx, leftYF + ly);
                }
                if (!isLeft || moveBoth) {
                    (fp)->setRightBezierPointAtTime(time, rightXF + rx, rightYF + ry);
                }
            }
            if (!isOnKeyframe) {
                keySet = true;
            }
        } else {
            ///this function is called when building a new bezier we must
            ///move the static position if there is no keyframe, otherwise the
            ///curve would never be built
            if (isLeft || moveBoth) {
                (cp)->setLeftBezierStaticPosition(leftX + lx, leftY + ly);
            }
            if (!isLeft || moveBoth) {
                (cp)->setRightBezierStaticPosition(rightX + rx, rightY + ry);
            }
            if (moveFeather && useFeatherPoints()) {
                if (isLeft || moveBoth) {
                    (fp)->setLeftBezierStaticPosition(leftXF + lx, leftYF + ly);
                }
                if (!isLeft || moveBoth) {
                    (fp)->setRightBezierStaticPosition(rightXF + rx, rightYF + ry);
                }
                    
            }
        }
        
        if (rippleEdit) {
            std::set<int> keyframes;
            _imp->getKeyframeTimes(&keyframes);
            for (std::set<int>::iterator it2 = keyframes.begin(); it2 != keyframes.end(); ++it2) {
                if (*it2 == time) {
                    continue;
                }
                
                if (isLeft || moveBoth) {
                    (cp)->getLeftBezierPointAtTime(*it2, &leftX, &leftY,true);
                    (cp)->setLeftBezierPointAtTime(*it2, leftX + lx, leftY + ly);
                    if (moveFeather && useFeatherPoints()) {
                        (fp)->getLeftBezierPointAtTime(*it2, &leftXF, &leftYF,true);
                        (fp)->setLeftBezierPointAtTime(*it2, leftXF + lx, leftYF + ly);
                    }
                } else {
                    (cp)->getRightBezierPointAtTime(*it2, &rightX, &rightY,true);
                    (cp)->setRightBezierPointAtTime(*it2, rightX + rx, rightY + ry);
                    if (moveFeather && useFeatherPoints()) {
                        (fp)->getRightBezierPointAtTime(*it2, &rightXF, &rightYF,true);
                        (fp)->setRightBezierPointAtTime(*it2, rightXF + rx, rightYF + ry);
                    }
                }
            }
        }
    }
    refreshPolygonOrientation(time);
    if (autoKeying) {
        setKeyframe(time);
    }
    if (keySet) {
        Q_EMIT keyframeSet(time);
    }

} // moveBezierPointInternal

void
Bezier::moveLeftBezierPoint(int index,
                            int time,
                            double dx,
                            double dy)
{
    moveBezierPointInternal(NULL, index, time, dx, dy, 0, 0, true, false);
} // moveLeftBezierPoint

void
Bezier::moveRightBezierPoint(int index,
                             int time,
                             double dx,
                             double dy)
{
    moveBezierPointInternal(NULL, index, time, 0, 0, dx, dy, false, false);
} // moveRightBezierPoint

void
Bezier::movePointLeftAndRightIndex(BezierCP & p,
                                   int time,
                                   double lx,
                                   double ly,
                                   double rx,
                                   double ry)
{
    moveBezierPointInternal(&p, -1, time, lx, ly, rx, ry, false, true);
}


void
Bezier::setPointAtIndexInternal(bool setLeft,bool setRight,bool setPoint,bool feather,bool featherAndCp,int index,int time,double x,double y,double lx,double ly,double rx,double ry)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    bool rippleEdit = getContext()->isRippleEditEnabled();
    bool keySet = false;
    
    {
        QMutexLocker l(&itemMutex);
        bool isOnKeyframe = _imp->hasKeyframeAtTime(time);
        
        if ( index >= (int)_imp->points.size() ) {
            throw std::invalid_argument("Bezier::setPointAtIndex: Index out of range.");
        }
        
        BezierCPs::iterator fp = _imp->featherPoints.begin();
        BezierCPs::iterator cp = _imp->points.begin();
        if (!feather && !featherAndCp) {
            fp = cp;
        }
        std::advance(fp, index);
        if (featherAndCp) {
            std::advance(cp, index);
        }
        
        if (autoKeying || isOnKeyframe) {
            if (setPoint) {
                (*fp)->setPositionAtTime(time, x, y);
                if (featherAndCp) {
                    (*cp)->setPositionAtTime(time, x, y);
                }
            }
            if (setLeft) {
                (*fp)->setLeftBezierPointAtTime(time, lx, ly);
                if (featherAndCp) {
                    (*cp)->setLeftBezierPointAtTime(time, lx, ly);
                }
            }
            if (setRight) {
                (*fp)->setRightBezierPointAtTime(time, rx, ry);
                if (featherAndCp) {
                    (*cp)->setRightBezierPointAtTime(time, rx, ry);
                }
            }
            if (!isOnKeyframe) {
                keySet = true;
            }
        }
        
        if (rippleEdit) {
            std::set<int> keyframes;
            _imp->getKeyframeTimes(&keyframes);
            for (std::set<int>::iterator it2 = keyframes.begin(); it2 != keyframes.end(); ++it2) {
                if (setPoint) {
                    (*fp)->setPositionAtTime(*it2, x, y);
                    if (featherAndCp) {
                        (*cp)->setPositionAtTime(*it2, x, y);
                    }
                }
                if (setLeft) {
                    (*fp)->setLeftBezierPointAtTime(*it2, lx, ly);
                    if (featherAndCp) {
                        (*cp)->setLeftBezierPointAtTime(*it2, lx, ly);
                    }
                }
                if (setRight) {
                    (*fp)->setRightBezierPointAtTime(*it2, rx, ry);
                    if (featherAndCp) {
                        (*cp)->setRightBezierPointAtTime(*it2, rx, ry);
                    }
                }
            }
        }
    }
    refreshPolygonOrientation(time);
    if (autoKeying) {
        setKeyframe(time);
    }
    if (keySet) {
        Q_EMIT keyframeSet(time);
    }

} // setPointAtIndexInternal

void
Bezier::setLeftBezierPoint(int index,
                           int time,
                           double x,
                           double y)
{
    setPointAtIndexInternal(true, false, false, false, true, index, time, 0, 0, x, y, 0, 0);
}

void
Bezier::setRightBezierPoint(int index,
                            int time,
                            double x,
                            double y)
{
    setPointAtIndexInternal(false, true, false, false, true, index, time, 0, 0, 0, 0, x, y);
}

void
Bezier::setPointAtIndex(bool feather,
                        int index,
                        int time,
                        double x,
                        double y,
                        double lx,
                        double ly,
                        double rx,
                        double ry)
{
    setPointAtIndexInternal(true, true, true, feather, false, index, time, x, y, lx, ly, rx, ry);
}



void
Bezier::transformPoint(const boost::shared_ptr<BezierCP> & point,
                       int time,
                       Transform::Matrix3x3* matrix)
{
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    bool keySet = false;
    {
        QMutexLocker l(&itemMutex);
        Transform::Point3D cp,leftCp,rightCp;
        point->getPositionAtTime(time, &cp.x, &cp.y,true);
        point->getLeftBezierPointAtTime(time, &leftCp.x, &leftCp.y,true);
        bool isonKeyframe = point->getRightBezierPointAtTime(time, &rightCp.x, &rightCp.y,true);
        
        
        cp.z = 1.;
        leftCp.z = 1.;
        rightCp.z = 1.;
        
        cp = matApply(*matrix, cp);
        leftCp = matApply(*matrix, leftCp);
        rightCp = matApply(*matrix, rightCp);
        
        cp.x /= cp.z; cp.y /= cp.z;
        leftCp.x /= leftCp.z; leftCp.y /= leftCp.z;
        rightCp.x /= rightCp.z; rightCp.y /= rightCp.z;
        
        if (autoKeying || isonKeyframe) {
            point->setPositionAtTime(time, cp.x, cp.y);
            point->setLeftBezierPointAtTime(time, leftCp.x, leftCp.y);
            point->setRightBezierPointAtTime(time, rightCp.x, rightCp.y);
            if (!isonKeyframe) {
                keySet = true;
            }
        }
    }
    
    refreshPolygonOrientation(time);
    if (keySet) {
        Q_EMIT keyframeSet(time);
    }
}


void
Bezier::removeFeatherAtIndex(int index)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    assert(useFeatherPoints());
    QMutexLocker l(&itemMutex);

    if ( index >= (int)_imp->points.size() ) {
        throw std::invalid_argument("Bezier::removeFeatherAtIndex: Index out of range.");
    }

    BezierCPs::iterator cp = _imp->atIndex(index);
    BezierCPs::iterator fp = _imp->featherPoints.begin();
    std::advance(fp, index);

    assert( cp != _imp->points.end() && fp != _imp->featherPoints.end() );

    (*fp)->clone(**cp);
    
}

void
Bezier::smoothOrCuspPointAtIndex(bool isSmooth,int index,int time,const std::pair<double,double>& pixelScale)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    bool keySet = false;
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    bool rippleEdit = getContext()->isRippleEditEnabled();
    
    {
        QMutexLocker l(&itemMutex);
        if ( index >= (int)_imp->points.size() ) {
            throw std::invalid_argument("Bezier::smoothOrCuspPointAtIndex: Index out of range.");
        }
        
        BezierCPs::iterator cp = _imp->atIndex(index);
        BezierCPs::iterator fp;
        
        bool useFeather = useFeatherPoints();
        
        if (useFeather) {
            fp = _imp->featherPoints.begin();
            std::advance(fp, index);
        }
        assert( cp != _imp->points.end() && fp != _imp->featherPoints.end() );
        if (isSmooth) {
            keySet = (*cp)->smoothPoint(time,autoKeying,rippleEdit,pixelScale);
            if (useFeather) {
                (*fp)->smoothPoint(time,autoKeying,rippleEdit,pixelScale);
            }
        } else {
            keySet = (*cp)->cuspPoint(time,autoKeying,rippleEdit, pixelScale);
            if (useFeather) {
                (*fp)->cuspPoint(time,autoKeying,rippleEdit,pixelScale);
            }
        }
    }
    refreshPolygonOrientation(time);
    if (autoKeying) {
        setKeyframe(time);
    }
    if (keySet) {
        Q_EMIT keyframeSet(time);
    }
}

void
Bezier::smoothPointAtIndex(int index,
                           int time,
                           const std::pair<double,double>& pixelScale)
{
    smoothOrCuspPointAtIndex(true, index, time, pixelScale);
}

void
Bezier::cuspPointAtIndex(int index,
                         int time,
                         const std::pair<double,double>& pixelScale)
{
    smoothOrCuspPointAtIndex(false, index, time, pixelScale);
}

void
Bezier::setKeyframe(int time)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    {
        QMutexLocker l(&itemMutex);
        if ( _imp->hasKeyframeAtTime(time) ) {
            return;
        }


        bool useFeather = useFeatherPoints();
        assert(_imp->points.size() == _imp->featherPoints.size() || !useFeather);

        for (BezierCPs::iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
            double x, y;
            double leftDerivX, rightDerivX, leftDerivY, rightDerivY;

            (*it)->getPositionAtTime(time, &x, &y,true);
            (*it)->setPositionAtTime(time, x, y);

            (*it)->getLeftBezierPointAtTime(time, &leftDerivX, &leftDerivY,true);
            (*it)->getRightBezierPointAtTime(time, &rightDerivX, &rightDerivY,true);
            (*it)->setLeftBezierPointAtTime(time, leftDerivX, leftDerivY);
            (*it)->setRightBezierPointAtTime(time, rightDerivX, rightDerivY);
        }

        if (useFeather) {
            for (BezierCPs::iterator it = _imp->featherPoints.begin(); it != _imp->featherPoints.end(); ++it) {
                double x, y;
                double leftDerivX, rightDerivX, leftDerivY, rightDerivY;

                (*it)->getPositionAtTime(time, &x, &y,true);
                (*it)->setPositionAtTime(time, x, y);

                (*it)->getLeftBezierPointAtTime(time, &leftDerivX, &leftDerivY,true);
                (*it)->getRightBezierPointAtTime(time, &rightDerivX, &rightDerivY,true);
                (*it)->setLeftBezierPointAtTime(time, leftDerivX, leftDerivY);
                (*it)->setRightBezierPointAtTime(time, rightDerivX, rightDerivY);
            }
        }
    }
    Q_EMIT keyframeSet(time);
}

void
Bezier::removeKeyframe(int time)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    {
        QMutexLocker l(&itemMutex);

        if ( !_imp->hasKeyframeAtTime(time) ) {
            return;
        }
        assert( _imp->featherPoints.size() == _imp->points.size() || !useFeatherPoints());

        bool useFeather = useFeatherPoints();
        
        BezierCPs::iterator fp = _imp->featherPoints.begin();
        for (BezierCPs::iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
            (*it)->removeKeyframe(time);
            if (useFeather) {
                (*fp)->removeKeyframe(time);
                ++fp;
            }
        }
        
        std::map<int,bool>::iterator found = _imp->isClockwiseOriented.find(time);
        if (found != _imp->isClockwiseOriented.end()) {
            _imp->isClockwiseOriented.erase(found);
        }
    }
    getContext()->evaluateChange();
    Q_EMIT keyframeRemoved(time);
}

void
Bezier::removeAnimation()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    int time = getContext()->getTimelineCurrentTime();
    {
        QMutexLocker l(&itemMutex);
        
        assert( _imp->featherPoints.size() == _imp->points.size() || !useFeatherPoints() );
        
        bool useFeather = useFeatherPoints();
        BezierCPs::iterator fp = _imp->featherPoints.begin();
        for (BezierCPs::iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
            (*it)->removeAnimation(time);
            if (useFeather) {
                (*fp)->removeAnimation(time);
                ++fp;
            }
        }
     
        _imp->isClockwiseOriented.clear();
    }
    Q_EMIT animationRemoved();
}

void
Bezier::moveKeyframe(int oldTime,int newTime)
{
    assert(QThread::currentThread() == qApp->thread());
    
    bool useFeather = useFeatherPoints();
    BezierCPs::iterator fp = _imp->featherPoints.begin();
    for (BezierCPs::iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
        double x,y,lx,ly,rx,ry;
        (*it)->getPositionAtTime(oldTime, &x, &y);
        (*it)->getLeftBezierPointAtTime(oldTime, &lx, &ly);
        (*it)->getRightBezierPointAtTime(oldTime, &rx, &ry);
        
        (*it)->removeKeyframe(oldTime);
        
        (*it)->setPositionAtTime(newTime, x, y);
        (*it)->setLeftBezierPointAtTime(newTime, lx, ly);
        (*it)->setRightBezierPointAtTime(newTime, rx, ry);
        
        if (useFeather) {
            (*fp)->getPositionAtTime(oldTime, &x, &y);
            (*fp)->getLeftBezierPointAtTime(oldTime, &lx, &ly);
            (*fp)->getRightBezierPointAtTime(oldTime, &rx, &ry);
            
            (*fp)->removeKeyframe(oldTime);
            
            (*fp)->setPositionAtTime(newTime, x, y);
            (*fp)->setLeftBezierPointAtTime(newTime, lx, ly);
            (*fp)->setRightBezierPointAtTime(newTime, rx, ry);
            ++fp;
        }
    }
    
    {
        QMutexLocker k(&itemMutex);
        bool foundOld;
        bool oldValue;
        std::map<int,bool>::iterator foundOldIt = _imp->isClockwiseOriented.find(oldTime);
        foundOld = foundOldIt != _imp->isClockwiseOriented.end();
        if (foundOld) {
            oldValue = foundOldIt->first;
        } else {
            oldValue = 0;
        }
        _imp->isClockwiseOriented[newTime] = oldValue;
    }
    
    Q_EMIT keyframeRemoved(oldTime);
    Q_EMIT keyframeSet(newTime);
}

int
Bezier::getKeyframesCount() const
{
    QMutexLocker l(&itemMutex);

    if ( _imp->points.empty() ) {
        return 0;
    } else {
        return _imp->points.front()->getKeyframesCount();
    }
}

void
Bezier::deCastelJau(const std::list<boost::shared_ptr<BezierCP> >& cps, int time,unsigned int mipMapLevel,
                    bool finished,
                 int nBPointsPerSegment, std::list<Natron::Point>* points, RectD* bbox)
{
    BezierCPs::const_iterator next = cps.begin();
    
    if (next != cps.end()) {
        ++next;
    }
    for (BezierCPs::const_iterator it = cps.begin(); it != cps.end(); ++it) {
        if ( next == cps.end() ) {
            if (!finished) {
                break;
            }
            next = cps.begin();
        }
        bezierSegmentEval(*(*it),*(*next), time,mipMapLevel, nBPointsPerSegment, points,bbox);
        
        // increment for next iteration
        if (next != cps.end()) {
            ++next;
        }
    } // for()
}

void
Bezier::evaluateAtTime_DeCasteljau(int time,
                                   unsigned int mipMapLevel,
                                   int nbPointsPerSegment,
                                   std::list< Natron::Point >* points,
                                   RectD* bbox) const
{
    QMutexLocker l(&itemMutex);
    deCastelJau(_imp->points, time, mipMapLevel, _imp->finished, nbPointsPerSegment, points, bbox);
}

void
Bezier::evaluateAtTime_DeCasteljau_autoNbPoints(int time,
                                             unsigned int mipMapLevel,
                                             std::list<Natron::Point>* points,
                                             RectD* bbox) const
{
    evaluateAtTime_DeCasteljau(time, mipMapLevel, -1, points, bbox);
}

void
Bezier::evaluateFeatherPointsAtTime_DeCasteljau(int time,
                                                unsigned int mipMapLevel,
                                                int nbPointsPerSegment,
                                                bool evaluateIfEqual, ///< evaluate only if feather points are different from control points
                                                std::list< Natron::Point >* points, ///< output
                                                RectD* bbox) const ///< output
{
    assert(useFeatherPoints());
    QMutexLocker l(&itemMutex);

    if ( _imp->points.empty() ) {
        return;
    }
    BezierCPs::const_iterator itCp = _imp->points.begin();
    BezierCPs::const_iterator next = _imp->featherPoints.begin();
    if (next != _imp->featherPoints.end()) {
        ++next;
    }
    BezierCPs::const_iterator nextCp = itCp;
    if (nextCp != _imp->points.end()) {
        ++nextCp;
    }
    for (BezierCPs::const_iterator it = _imp->featherPoints.begin(); it != _imp->featherPoints.end();
         ++it) {
        if ( next == _imp->featherPoints.end() ) {
            next = _imp->featherPoints.begin();
        }
        if ( nextCp == _imp->points.end() ) {
            if (!_imp->finished) {
                break;
            }
            nextCp = _imp->points.begin();
        }
        if ( !evaluateIfEqual && bezierSegmenEqual(time, **itCp, **nextCp, **it, **next) ) {
            continue;
        }

        bezierSegmentEval(*(*it),*(*next), time, mipMapLevel, nbPointsPerSegment, points, bbox);

        // increment for next iteration
        if (itCp != _imp->featherPoints.end()) {
            ++itCp;
        }
        if (next != _imp->featherPoints.end()) {
            ++next;
        }
        if (nextCp != _imp->featherPoints.end()) {
            ++nextCp;
        }
    } // for(it)
}

RectD
Bezier::getBoundingBox(int time) const
{
    
    std::list<Point> pts;
    RectD bbox; // a very empty bbox

    bbox.x1 = std::numeric_limits<double>::infinity();
    bbox.x2 = -std::numeric_limits<double>::infinity();
    bbox.y1 = std::numeric_limits<double>::infinity();
    bbox.y2 = -std::numeric_limits<double>::infinity();
    
    QMutexLocker l(&itemMutex);
    bezierSegmentListBboxUpdate(_imp->points, _imp->finished, time, 0, &bbox);
    
    
    if (useFeatherPoints()) {
        bezierSegmentListBboxUpdate(_imp->featherPoints, _imp->finished, time, 0, &bbox);
        // EDIT: Partial fix, just pad the BBOX by the feather distance. This might not be accurate but gives at least something
        // enclosing the real bbox and close enough
        double featherDistance = getFeatherDistance(time);
        bbox.x1 -= featherDistance;
        bbox.x2 += featherDistance;
        bbox.y1 -= featherDistance;
        bbox.y2 += featherDistance;
    }
    return bbox;
}

const std::list< boost::shared_ptr<BezierCP> > &
Bezier::getControlPoints() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->points;
}

//protected only
std::list< boost::shared_ptr<BezierCP> > &
Bezier::getControlPoints_internal()
{
    return _imp->points;
}

std::list< boost::shared_ptr<BezierCP> >
Bezier::getControlPoints_mt_safe() const
{
    QMutexLocker l(&itemMutex);

    return _imp->points;
}

const std::list< boost::shared_ptr<BezierCP> > &
Bezier::getFeatherPoints() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->featherPoints;
}

std::list< boost::shared_ptr<BezierCP> >
Bezier::getFeatherPoints_mt_safe() const
{
    QMutexLocker l(&itemMutex);
    return _imp->featherPoints;
}


std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> >
Bezier::isNearbyControlPoint(double x,
                             double y,
                             double acceptance,
                             ControlPointSelectionPrefEnum pref,
                             int* index) const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    int time = getContext()->getTimelineCurrentTime();
    QMutexLocker l(&itemMutex);
    boost::shared_ptr<BezierCP> cp,fp;

    switch (pref) {
    case eControlPointSelectionPrefFeatherFirst: {
        BezierCPs::const_iterator itF = _imp->findFeatherPointNearby(x, y, acceptance, time, index);
        if ( itF != _imp->featherPoints.end() ) {
            fp = *itF;
            BezierCPs::const_iterator it = _imp->points.begin();
            std::advance(it, *index);
            cp = *it;

            return std::make_pair(fp, cp);
        } else {
            BezierCPs::const_iterator it = _imp->findControlPointNearby(x, y, acceptance, time, index);
            if ( it != _imp->points.end() ) {
                cp = *it;
                itF = _imp->featherPoints.begin();
                std::advance(itF, *index);
                fp = *itF;

                return std::make_pair(cp, fp);
            }
        }
        break;
    }
    case eControlPointSelectionPrefControlPointFirst:
    case eControlPointSelectionPrefWhateverFirst:
    default: {
        BezierCPs::const_iterator it = _imp->findControlPointNearby(x, y, acceptance, time, index);
        if ( it != _imp->points.end() ) {
            cp = *it;
            BezierCPs::const_iterator itF = _imp->featherPoints.begin();
            std::advance(itF, *index);
            fp = *itF;

            return std::make_pair(cp, fp);
        } else {
            BezierCPs::const_iterator itF = _imp->findFeatherPointNearby(x, y, acceptance, time, index);
            if ( itF != _imp->featherPoints.end() ) {
                fp = *itF;
                it = _imp->points.begin();
                std::advance(it, *index);
                cp = *it;

                return std::make_pair(fp, cp);
            }
        }
        break;
    }
    }

    ///empty pair
    *index = -1;

    return std::make_pair(cp,fp);
} // isNearbyControlPoint

int
Bezier::getControlPointIndex(const boost::shared_ptr<BezierCP> & cp) const
{
    return getControlPointIndex( cp.get() );
}

int
Bezier::getControlPointIndex(const BezierCP* cp) const
{
    ///only called on the main-thread
    assert(cp);
    QMutexLocker l(&itemMutex);
    int i = 0;
    for (BezierCPs::const_iterator it = _imp->points.begin(); it != _imp->points.end(); ++it, ++i) {
        if (it->get() == cp) {
            return i;
        }
    }

    return -1;
}

int
Bezier::getFeatherPointIndex(const boost::shared_ptr<BezierCP> & fp) const
{
    ///only called on the main-thread
    QMutexLocker l(&itemMutex);
    int i = 0;

    for (BezierCPs::const_iterator it = _imp->featherPoints.begin(); it != _imp->featherPoints.end(); ++it, ++i) {
        if (*it == fp) {
            return i;
        }
    }

    return -1;
}

boost::shared_ptr<BezierCP>
Bezier::getControlPointAtIndex(int index) const
{
    QMutexLocker l(&itemMutex);

    if ( (index < 0) || ( index >= (int)_imp->points.size() ) ) {
        return boost::shared_ptr<BezierCP>();
    }

    BezierCPs::const_iterator it = _imp->points.begin();
    std::advance(it, index);

    return *it;
}

boost::shared_ptr<BezierCP>
Bezier::getFeatherPointAtIndex(int index) const
{
    QMutexLocker l(&itemMutex);

    if ( (index < 0) || ( index >= (int)_imp->featherPoints.size() ) ) {
        return boost::shared_ptr<BezierCP>();
    }

    BezierCPs::const_iterator it = _imp->featherPoints.begin();
    std::advance(it, index);

    return *it;
}

std::list< std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> > >
Bezier::controlPointsWithinRect(double l,
                                double r,
                                double b,
                                double t,
                                double acceptance,
                                int mode) const
{
    std::list< std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> > > ret;

    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker locker(&itemMutex);
    int time = getContext()->getTimelineCurrentTime();
    int i = 0;
    if ( (mode == 0) || (mode == 1) ) {
        for (BezierCPs::const_iterator it = _imp->points.begin(); it != _imp->points.end(); ++it, ++i) {
            double x,y;
            (*it)->getPositionAtTime(time, &x, &y);
            if ( ( x >= (l - acceptance) ) && ( x <= (r + acceptance) ) && ( y >= (b - acceptance) ) && ( y <= (t - acceptance) ) ) {
                std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> > p;
                p.first = *it;
                BezierCPs::const_iterator itF = _imp->featherPoints.begin();
                std::advance(itF, i);
                p.second = *itF;
                ret.push_back(p);
            }
        }
    }
    i = 0;
    if ( (mode == 0) || (mode == 2) ) {
        for (BezierCPs::const_iterator it = _imp->featherPoints.begin(); it != _imp->featherPoints.end(); ++it, ++i) {
            double x,y;
            (*it)->getPositionAtTime(time, &x, &y);
            if ( ( x >= (l - acceptance) ) && ( x <= (r + acceptance) ) && ( y >= (b - acceptance) ) && ( y <= (t - acceptance) ) ) {
                std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> > p;
                p.first = *it;
                BezierCPs::const_iterator itF = _imp->points.begin();
                std::advance(itF, i);
                p.second = *itF;

                ///avoid duplicates
                bool found = false;
                for (std::list< std::pair<boost::shared_ptr<BezierCP>,boost::shared_ptr<BezierCP> > >::iterator it2 = ret.begin();
                     it2 != ret.end(); ++it2) {
                    if (it2->first == *itF) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ret.push_back(p);
                }
            }
        }
    }

    return ret;
} // controlPointsWithinRect

boost::shared_ptr<BezierCP>
Bezier::getFeatherPointForControlPoint(const boost::shared_ptr<BezierCP> & cp) const
{
    assert( !cp->isFeatherPoint() );
    int index = getControlPointIndex(cp);
    assert(index != -1);

    return getFeatherPointAtIndex(index);
}

boost::shared_ptr<BezierCP>
Bezier::getControlPointForFeatherPoint(const boost::shared_ptr<BezierCP> & fp) const
{
    assert( fp->isFeatherPoint() );
    int index = getFeatherPointIndex(fp);
    assert(index != -1);

    return getControlPointAtIndex(index);
}

void
Bezier::leftDerivativeAtPoint(int time,
                              const BezierCP & p,
                              const BezierCP & prev,
                              double *dx,
                              double *dy)
{
    ///First-off, determine if the segment is a linear/quadratic/cubic bezier segment.
    assert( !p.equalsAtTime(time, prev) );
    bool p0equalsP1,p1equalsP2,p2equalsP3;
    Point p0,p1,p2,p3;
    prev.getPositionAtTime(time, &p0.x, &p0.y);
    prev.getRightBezierPointAtTime(time, &p1.x, &p1.y);
    p.getLeftBezierPointAtTime(time, &p2.x, &p2.y);
    p.getPositionAtTime(time, &p3.x, &p3.y);
    p0equalsP1 = p0.x == p1.x && p0.y == p1.y;
    p1equalsP2 = p1.x == p2.x && p1.y == p2.y;
    p2equalsP3 = p2.x == p3.x && p2.y == p3.y;
    int degree = 3;
    if (p0equalsP1) {
        --degree;
    }
    if (p1equalsP2) {
        --degree;
    }
    if (p2equalsP3) {
        --degree;
    }
    assert(degree >= 1 && degree <= 3);

    ///derivatives for t == 1.
    if (degree == 1) {
        *dx = p0.x - p3.x;
        *dy = p0.y - p3.y;
    } else if (degree == 2) {
        if (p0equalsP1) {
            p1 = p2;
        }
        *dx = 2. * (p1.x - p3.x);
        *dy = 2. * (p1.y - p3.y);
    } else {
        *dx = 3. * (p2.x - p3.x);
        *dy = 3. * (p2.y - p3.y);
    }
}

void
Bezier::rightDerivativeAtPoint(int time,
                               const BezierCP & p,
                               const BezierCP & next,
                               double *dx,
                               double *dy)
{
    ///First-off, determine if the segment is a linear/quadratic/cubic bezier segment.
    assert( !p.equalsAtTime(time, next) );
    bool p0equalsP1,p1equalsP2,p2equalsP3;
    Point p0,p1,p2,p3;
    p.getPositionAtTime(time, &p0.x, &p0.y);
    p.getRightBezierPointAtTime(time, &p1.x, &p1.y);
    next.getLeftBezierPointAtTime(time, &p2.x, &p2.y);
    next.getPositionAtTime(time, &p3.x, &p3.y);
    p0equalsP1 = p0.x == p1.x && p0.y == p1.y;
    p1equalsP2 = p1.x == p2.x && p1.y == p2.y;
    p2equalsP3 = p2.x == p3.x && p2.y == p3.y;
    int degree = 3;
    if (p0equalsP1) {
        --degree;
    }
    if (p1equalsP2) {
        --degree;
    }
    if (p2equalsP3) {
        --degree;
    }
    assert(degree >= 1 && degree <= 3);

    ///derivatives for t == 0.
    if (degree == 1) {
        *dx = p3.x - p0.x;
        *dy = p3.y - p0.y;
    } else if (degree == 2) {
        if (p0equalsP1) {
            p1 = p2;
        }
        *dx = 2. * (p1.x - p0.x);
        *dy = 2. * (p1.y - p0.y);
    } else {
        *dx = 3. * (p1.x - p0.x);
        *dy = 3. * (p1.y - p0.y);
    }
}

void
Bezier::save(RotoItemSerialization* obj) const
{
    BezierSerialization* s = dynamic_cast<BezierSerialization*>(obj);
    if (s) {
        QMutexLocker l(&itemMutex);

        s->_closed = _imp->finished;

        assert( _imp->featherPoints.size() == _imp->points.size() || !useFeatherPoints());


        bool useFeather = useFeatherPoints();
        BezierCPs::const_iterator fp = _imp->featherPoints.begin();
        for (BezierCPs::const_iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
            BezierCP c;
            c.clone(**it);
            s->_controlPoints.push_back(c);
            if (useFeather) {
                BezierCP f;
                f.clone(**fp);
                s->_featherPoints.push_back(f);
                ++fp;
            }
            
        }
        s->_isStroke = dynamic_cast<const RotoStrokeItem*>(this) != 0;
    }
    
    RotoDrawableItem::save(obj);
}

void
Bezier::load(const RotoItemSerialization & obj)
{
    boost::shared_ptr<Bezier> this_shared = boost::dynamic_pointer_cast<Bezier>(shared_from_this());
    assert(this_shared);

    const BezierSerialization & s = dynamic_cast<const BezierSerialization &>(obj);
    {
        QMutexLocker l(&itemMutex);
        _imp->finished = s._closed;
        
        bool useFeather = useFeatherPoints();
        std::list<BezierCP>::const_iterator itF = s._featherPoints.begin();
        for (std::list<BezierCP>::const_iterator it = s._controlPoints.begin(); it != s._controlPoints.end(); ++it) {
            boost::shared_ptr<BezierCP> cp( new BezierCP(this_shared) );
            cp->clone(*it);
            _imp->points.push_back(cp);
            
            if (useFeather) {
                boost::shared_ptr<BezierCP> fp( new FeatherPoint(this_shared) );
                fp->clone(*itF);
                _imp->featherPoints.push_back(fp);
                ++itF;
            }
        }
    }
    refreshPolygonOrientation();
    RotoDrawableItem::load(obj);
}

void
Bezier::getKeyframeTimes(std::set<int> *times) const
{
    QMutexLocker l(&itemMutex);

    _imp->getKeyframeTimes(times);
}

void
Bezier::getKeyframeTimesAndInterpolation(std::list<std::pair<int,Natron::KeyframeTypeEnum> > *keys) const
{
    QMutexLocker l(&itemMutex);
    if ( _imp->points.empty() ) {
        return;
    }
    _imp->points.front()->getKeyFrames(keys);
}

int
Bezier::getPreviousKeyframeTime(int time) const
{
    std::set<int> times;
    QMutexLocker l(&itemMutex);

    _imp->getKeyframeTimes(&times);
    for (std::set<int>::reverse_iterator it = times.rbegin(); it != times.rend(); ++it) {
        if (*it < time) {
            return *it;
        }
    }

    return INT_MIN;
}

int
Bezier::getNextKeyframeTime(int time) const
{
    std::set<int> times;
    QMutexLocker l(&itemMutex);

    _imp->getKeyframeTimes(&times);
    for (std::set<int>::iterator it = times.begin(); it != times.end(); ++it) {
        if (*it > time) {
            return *it;
        }
    }

    return INT_MAX;
}

int
Bezier::getKeyFrameIndex(double time) const
{
    QMutexLocker l(&itemMutex);
    if (_imp->points.empty()) {
        return -1;
    }
    return _imp->points.front()->getKeyFrameIndex(time);
}

void
Bezier::setKeyFrameInterpolation(Natron::KeyframeTypeEnum interp,int index)
{
    QMutexLocker l(&itemMutex);
    bool useFeather = useFeatherPoints();
    BezierCPs::iterator fp = _imp->featherPoints.begin();
    for (BezierCPs::iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
        (*it)->setKeyFrameInterpolation(interp, index);
        
        if (useFeather) {
            (*fp)->setKeyFrameInterpolation(interp, index);
            ++fp;
        }
    }
}


void
Bezier::point_line_intersection(const Point &p1,
                        const Point &p2,
                        const Point &pos,
                        int *winding)
{
    double x1 = p1.x;
    double y1 = p1.y;
    double x2 = p2.x;
    double y2 = p2.y;
    double y = pos.y;
    int dir = 1;

    if ( qFuzzyCompare(y1, y2) ) {
        // ignore horizontal lines according to scan conversion rule
        return;
    } else if (y2 < y1) {
        double x_tmp = x2; x2 = x1; x1 = x_tmp;
        double y_tmp = y2; y2 = y1; y1 = y_tmp;
        dir = -1;
    }

    if ( (y >= y1) && (y < y2) ) {
        double x = x1 + ( (x2 - x1) / (y2 - y1) ) * (y - y1);

        // count up the winding number if we're
        if (x <= pos.x) {
            (*winding) += dir;
        }
    }
}

static bool
pointInPolygon(const Point & p,
               const std::list<Point> & polygon,
               const RectD & featherPolyBBox,
               Bezier::FillRuleEnum rule)
{
    ///first check if the point lies inside the bounding box
    if ( (p.x < featherPolyBBox.x1) || (p.x >= featherPolyBBox.x2) || (p.y < featherPolyBBox.y1) || (p.y >= featherPolyBBox.y2)
         || polygon.empty() ) {
        return false;
    }

    int winding_number = 0;
    std::list<Point>::const_iterator last_pt = polygon.begin();
    std::list<Point>::const_iterator last_start = last_pt;
    std::list<Point>::const_iterator cur = last_pt;
    ++cur;
    for (; cur != polygon.end(); ++cur, ++last_pt) {
        Bezier::point_line_intersection(*last_pt, *cur, p, &winding_number);
    }

    // implicitly close last subpath
    if (last_pt != last_start) {
        Bezier::point_line_intersection(*last_pt, *last_start, p, &winding_number);
    }

    return rule == Bezier::eFillRuleWinding
           ? (winding_number != 0)
           : ( (winding_number % 2) != 0 );
}

bool
Bezier::isFeatherPolygonClockwiseOrientedInternal(int time) const
{
    std::map<int,bool>::iterator it = _imp->isClockwiseOriented.find(time);
    if (it != _imp->isClockwiseOriented.end()) {
        return it->second;
    } else {
        
        int kfCount;
        if ( _imp->points.empty() ) {
            kfCount = 0;
        } else {
            kfCount = _imp->points.front()->getKeyframesCount();
        }
        if (kfCount > 0 && _imp->finished) {
            computePolygonOrientation(time, false);
            it = _imp->isClockwiseOriented.find(time);
            assert(it != _imp->isClockwiseOriented.end());
            return it->second;
        } else {
            return _imp->isClockwiseOrientedStatic;
        }
    }
}

bool
Bezier::isFeatherPolygonClockwiseOriented(int time) const
{
    QMutexLocker k(&itemMutex);
    return isFeatherPolygonClockwiseOrientedInternal(time);
}

void
Bezier::setAutoOrientationComputation(bool autoCompute)
{
    assert(QThread::currentThread() == qApp->thread());
    _imp->autoRecomputeOrientation = autoCompute;
}

void
Bezier::refreshPolygonOrientation(int time)
{
    QMutexLocker k(&itemMutex);
    if (!_imp->autoRecomputeOrientation) {
        return;
    }
    computePolygonOrientation(time,false);
}


void
Bezier::refreshPolygonOrientation()
{
    {
        QMutexLocker k(&itemMutex);
        if (!_imp->autoRecomputeOrientation) {
            return;
        }
    }
    std::set<int> kfs;
    getKeyframeTimes(&kfs);
    
    QMutexLocker k(&itemMutex);
    if (kfs.empty()) {
        computePolygonOrientation(0,true);
    } else {
        for (std::set<int>::iterator it = kfs.begin(); it != kfs.end(); ++it) {
            computePolygonOrientation(*it,false);
        }
    }
}

/*
 The algorithm to know which side is the outside of a polygon consists in computing the global polygon orientation.
 To compute the orientation, compute its surface. If positive the polygon is clockwise, if negative it's counterclockwise.
 to compute the surface, take the starting point of the polygon, and imagine a fan made of all the triangles
 pointing at this point. The surface of a tringle is half the cross-product of two of its sides issued from
 the same point (the starting point of the polygon, in this case.
 The orientation of a polygon has to be computed only once for each modification of the polygon (whenever it's edited), and
 should be stored with the polygon.
 Of course an 8-shaped polygon doesn't have an outside, but it still has an orientation. The feather direction
 should follow this orientation.
 */
void
Bezier::computePolygonOrientation(int time,bool isStatic) const
{
    //Private - should already be locked
    assert(!itemMutex.tryLock());
    
    if (_imp->points.size() <= 1) {
        return;
    }
    
    bool useFeather = useFeatherPoints();
    const BezierCPs& cps = useFeather ? _imp->featherPoints : _imp->points;
    
    
    double polygonSurface = 0.;
    if (_imp->points.size() == 2) {
        //It does not matter since there are only 2 points
        polygonSurface = -1;
    } else {
        Point originalPoint;
        BezierCPs::const_iterator it = cps.begin();
        (*it)->getPositionAtTime(time, &originalPoint.x, &originalPoint.y);
        ++it;
        BezierCPs::const_iterator next = it;
        if (next != cps.end()) {
            ++next;
        }
        for (;next != cps.end(); ++it, ++next) {
            assert(it != cps.end());
            double x,y;
            (*it)->getPositionAtTime(time, &x, &y);
            double xN,yN;
            (*next)->getPositionAtTime(time, &xN, &yN);
            Point u;
            u.x = x - originalPoint.x;
            u.y = y - originalPoint.y;
            
            Point v;
            v.x = xN - originalPoint.x;
            v.y = yN - originalPoint.y;
            
            //This is the area of the parallelogram defined by the U and V sides
            //Since a triangle is half a parallelogram, just half the cross-product
            double crossProduct = v.y * u.x - v.x * u.y;
            polygonSurface += (crossProduct / 2.);
        }
    } // for()
    if (isStatic) {
        _imp->isClockwiseOrientedStatic = polygonSurface < 0;
    } else {
        _imp->isClockwiseOriented[time] = polygonSurface < 0;
    }
}

/**
 * @brief Computes the location of the feather extent relative to the current feather point position and
 * the given feather distance.
 * In the case the control point and the feather point of the bezier are distinct, this function just makes use
 * of Thales theorem.
 * If the feather point and the control point are equal then this function computes the left and right derivative
 * of the bezier at that point to determine the direction in which the extent is.
 * @returns The delta from the given feather point to apply to find out the extent position.
 *
 * Note that the delta will be applied to fp.
 **/
Point
Bezier::expandToFeatherDistance(const Point & cp, //< the point
                                Point* fp, //< the feather point
                                double featherDistance, //< feather distance
                                int time, //< time
                                bool clockWise, //< is the bezier  clockwise oriented or not
                                BezierCPs::const_iterator prevFp, //< iterator pointing to the feather before curFp
                                BezierCPs::const_iterator curFp, //< iterator pointing to fp
                                BezierCPs::const_iterator nextFp) //< iterator pointing after curFp
{
    Point ret;

    if (featherDistance != 0) {
        ///shortcut when the feather point is different than the control point
        if ( (cp.x != fp->x) && (cp.y != fp->y) ) {
            double dx = (fp->x - cp.x);
            double dy = (fp->y - cp.y);
            double dist = sqrt(dx * dx + dy * dy);
            ret.x = ( dx * (dist + featherDistance) ) / dist;
            ret.y = ( dy * (dist + featherDistance) ) / dist;
            fp->x =  ret.x + cp.x;
            fp->y =  ret.y + cp.y;
        } else {
            //compute derivatives to determine the feather extent
            double leftX,leftY,rightX,rightY,norm;
            Bezier::leftDerivativeAtPoint(time, **curFp, **prevFp, &leftX, &leftY);
            Bezier::rightDerivativeAtPoint(time, **curFp, **nextFp, &rightX, &rightY);
            norm = sqrt( (rightX - leftX) * (rightX - leftX) + (rightY - leftY) * (rightY - leftY) );

            ///normalize derivatives by their norm
            if (norm != 0) {
                ret.x = -( (rightY - leftY) / norm );
                ret.y = ( (rightX - leftX) / norm );
            } else {
                ///both derivatives are the same, use the direction of the left one
                norm = sqrt( (leftX - cp.x) * (leftX - cp.x) + (leftY - cp.y) * (leftY - cp.y) );
                if (norm != 0) {
                    ret.x = -( (leftY - cp.y) / norm );
                    ret.y = ( (leftX - cp.x) / norm );
                } else {
                    ///both derivatives and control point are equal, just use 0
                    ret.x = ret.y = 0;
                }
            }

            if (clockWise) {
                fp->x = cp.x + ret.x * featherDistance;
                fp->y = cp.y + ret.y * featherDistance;
            } else {
                fp->x = cp.x - ret.x * featherDistance;
                fp->y = cp.y - ret.y * featherDistance;
            }
        }
    } else {
        ret.x = ret.y = 0;
    }

    return ret;
} // expandToFeatherDistance


////////////////////////////////////Stroke//////////////////////////////////

RotoStrokeItem::RotoStrokeItem(Natron::RotoStrokeType type,
                               const boost::shared_ptr<RotoContext>& context,
                               const std::string & name,
                               const boost::shared_ptr<RotoLayer>& parent)
#ifdef ROTO_STROKE_USE_FIT_CURVE
: Bezier(context,name,parent)
#else
: RotoDrawableItem(context,name,parent)
#endif
, _imp(new RotoStrokeItemPrivate(type))
{
    addKnob(_imp->brushSpacing);
    addKnob(_imp->brushSize);
    addKnob(_imp->brushHardness);
    addKnob(_imp->effectStrength);
    addKnob(_imp->visiblePortion);
}

RotoStrokeItem::~RotoStrokeItem()
{
    
}

Natron::RotoStrokeType
RotoStrokeItem::getBrushType() const
{
    return _imp->type;
}

void
RotoStrokeItem::initialize(const std::list<std::pair<Natron::Point,double> >& rawPoints)
{
    assert(QThread::currentThread() == qApp->thread());
#ifdef ROTO_STROKE_USE_FIT_CURVE

    clearAllPoints();
    
    std::vector<FitCurve::SimpleBezierCP> fitBezier;
    std::vector<Point> pointsVec;
    for (std::list<std::pair<Natron::Point,double> >::const_iterator it = rawPoints.begin(); it!=rawPoints.end(); ++it) {
        pointsVec.push_back(it->first);
    }
    
    //Fit the raw points to a cubic bezier
    FitCurve::fit_cubic(pointsVec, 0.5, &fitBezier);
    
    int keyframeTime = getContext()->getTimelineCurrentTime();
    bool autoKeying = getContext()->isAutoKeyingEnabled();
    
    boost::shared_ptr<Bezier> this_shared = boost::dynamic_pointer_cast<Bezier>(shared_from_this());
    assert(this_shared);
    
    BezierCPs& cps = getControlPoints_internal();
    
    //Fill the animating control points with the simple bezier curve
    for (std::vector<FitCurve::SimpleBezierCP>::iterator it = fitBezier.begin(); it!=fitBezier.end(); ++it) {
        boost::shared_ptr<BezierCP> p( new BezierCP(this_shared));
        if (autoKeying) {
            p->setPositionAtTime(keyframeTime, it->p.x, it->p.y);
            p->setLeftBezierPointAtTime(keyframeTime, it->leftTan.x, it->leftTan.y);
            p->setRightBezierPointAtTime(keyframeTime, it->rightTan.x, it->rightTan.y);
        } else {
            p->setStaticPosition(it->p.x, it->p.y);
            p->setLeftBezierStaticPosition(it->leftTan.x, it->leftTan.y);
            p->setRightBezierStaticPosition(it->rightTan.x, it->rightTan.y);
        }
        cps.insert(cps.end(),p);
    }
#else
    QMutexLocker k(&itemMutex);
    
    _imp->xCurve.clearKeyFrames();
    _imp->yCurve.clearKeyFrames();
    _imp->pressureCurve.clearKeyFrames();
    int i = 0;
    for (std::list<std::pair<Natron::Point,double> >::const_iterator it = rawPoints.begin();it!=rawPoints.end();++it,++i) {
        {
            KeyFrame k;
            k.setTime(i);
            k.setValue(it->first.x);
            _imp->xCurve.addKeyFrame(k);
        }
        {
            KeyFrame k;
            k.setTime(i);
            k.setValue(it->first.y);
            _imp->yCurve.addKeyFrame(k);
        }

        {
            KeyFrame k;
            k.setTime(i);
            k.setValue(it->second);
            _imp->pressureCurve.addKeyFrame(k);
        }

        
    }
#endif
}


void
RotoStrokeItem::clone(const RotoItem* other)
{
#ifdef ROTO_STROKE_USE_FIT_CURVE
    Bezier::clone(other);
#else
    const RotoStrokeItem* otherStroke = dynamic_cast<const RotoStrokeItem*>(other);
    assert(otherStroke);
    {
        QMutexLocker k(&itemMutex);
        _imp->xCurve.clone(otherStroke->_imp->xCurve);
        _imp->yCurve.clone(otherStroke->_imp->yCurve);
        _imp->pressureCurve.clone(otherStroke->_imp->pressureCurve);
        _imp->type = otherStroke->_imp->type;
    }
    RotoDrawableItem::clone(other);
#endif
}

void
RotoStrokeItem::save(RotoItemSerialization* obj) const
{
#ifdef ROTO_STROKE_USE_FIT_CURVE
    Bezier::save(obj);
#else
    RotoDrawableItem::save(obj);
#endif
    RotoStrokeSerialization* s = dynamic_cast<RotoStrokeSerialization*>(obj);
    assert(s);
    {
        QMutexLocker k(&itemMutex);
        s->_brushType = (int)_imp->type;
#ifndef ROTO_STROKE_USE_FIT_CURVE
        s->_xCurve = _imp->xCurve;
        s->_yCurve = _imp->yCurve;
        s->_pressureCurve = _imp->pressureCurve;
#endif
    }
    serializeRotoKnob(_imp->brushSize, &s->_brushSize);
    serializeRotoKnob(_imp->brushSpacing, &s->_brushSpacing);
    serializeRotoKnob(_imp->brushHardness, &s->_brushHardness);
    serializeRotoKnob(_imp->effectStrength, &s->_brushEffectStrength);
    serializeRotoKnob(_imp->visiblePortion, &s->_brushVisiblePortion);
    
}

void
RotoStrokeItem::load(const RotoItemSerialization & obj)
{
#ifdef ROTO_STROKE_USE_FIT_CURVE
    Bezier::load(obj);
#else
    RotoDrawableItem::load(obj);
#endif
    const RotoStrokeSerialization* s = dynamic_cast<const RotoStrokeSerialization*>(&obj);
    assert(s);
    {
        QMutexLocker k(&itemMutex);
        _imp->type = (Natron::RotoStrokeType)s->_brushType;
#ifndef ROTO_STROKE_USE_FIT_CURVE
        _imp->xCurve.clone(s->_xCurve);
        _imp->yCurve.clone(s->_yCurve);
        _imp->pressureCurve.clone(s->_pressureCurve);
#endif
    }
    _imp->brushSize->clone(s->_brushSize.getKnob().get());
    _imp->brushSpacing->clone(s->_brushSpacing.getKnob().get());
    _imp->brushHardness->clone(s->_brushHardness.getKnob().get());
    _imp->effectStrength->clone(s->_brushEffectStrength.getKnob().get());
    _imp->visiblePortion->clone(s->_brushVisiblePortion.getKnob().get());
}

RectD
RotoStrokeItem::getBoundingBox(int time) const
{
#ifdef ROTO_STROKE_USE_FIT_CURVE
    RectD ret = Bezier::getBoundingBox(time);
    double brushSize = _imp->brushSize->getValue();
    double hardness = _imp->brushHardness->getValue();
    double externalDotRadius = std::max(std::min(brushSize / 2. * hardness + brushSize / 2.,brushSize), 1.) / 2.;
    ret.x1 -= externalDotRadius;
    ret.y1 -= externalDotRadius;
    ret.y2 += externalDotRadius;
    ret.x2 += externalDotRadius;
    return ret;
#else
    QMutexLocker k(&itemMutex);
    
    (void)time;
    RectD bbox;
    bbox.x1 = std::numeric_limits<double>::infinity();
    bbox.x2 = -std::numeric_limits<double>::infinity();
    bbox.y1 = std::numeric_limits<double>::infinity();
    bbox.y2 = -std::numeric_limits<double>::infinity();
    
    assert(_imp->xCurve.getKeyFramesCount() == _imp->yCurve.getKeyFramesCount());
    int nKfs = _imp->xCurve.getKeyFramesCount();
    for (int i = 0; i < nKfs; ++i) {
        KeyFrame xK,yK;
        bool ok = _imp->xCurve.getKeyFrameWithIndex(i, &xK);
        assert(ok);
        ok = _imp->yCurve.getKeyFrameWithIndex(i, &yK);
        assert(ok);
        bbox.x1 = std::min(bbox.x1, xK.getValue());
        bbox.x2 = std::max(bbox.x2, xK.getValue());
        bbox.y1 = std::min(bbox.y1, yK.getValue());
        bbox.y2 = std::max(bbox.y2, yK.getValue());
    }
    double brushSize = _imp->brushSize->getValue() / 2.;
    
    bbox.x1 -= brushSize;
    bbox.x2 += brushSize;
    bbox.y1 -= brushSize;
    bbox.y2 += brushSize;
    return bbox;
#endif
}

#ifndef ROTO_STROKE_USE_FIT_CURVE
void
RotoStrokeItem::evaluateStroke(unsigned int mipMapLevel, std::list<std::pair<Natron::Point,double> >* points) const
{
    QMutexLocker k(&itemMutex);
    int nKfs = _imp->xCurve.getKeyFramesCount();
    for (int i = 0; i < nKfs - 1; ++i) {
        KeyFrame xK,yK,pK;
        bool ok = _imp->xCurve.getKeyFrameWithIndex(i, &xK);
        assert(ok);
        ok = _imp->yCurve.getKeyFrameWithIndex(i, &yK);
        assert(ok);
        ok = _imp->pressureCurve.getKeyFrameWithIndex(i, &pK);
        assert(ok);
        
        int nextIdx = (i + 1);
        assert(nextIdx < nKfs);
        
        KeyFrame xNext,yNext,pNext;
        ok = _imp->xCurve.getKeyFrameWithIndex(nextIdx, &xNext);
        assert(ok);
        ok = _imp->yCurve.getKeyFrameWithIndex(nextIdx, &yNext);
        assert(ok);
        ok = _imp->pressureCurve.getKeyFrameWithIndex(nextIdx, &pNext);
        assert(ok);
        
        double x1,y1,press1,x2,y2,press2;
        double x1pr,y1pr,x2pl,y2pl,press1pr,press2pl;
        x1 = xK.getValue();
        y1 = yK.getValue();
        press1 = pK.getValue();
        x2 = xNext.getValue();
        y2 = yNext.getValue();
        press2 = pNext.getValue();
        
        x1pr = x1 + xK.getRightDerivative() / 3.;
        y1pr = y1 + yK.getRightDerivative() / 3.;
        press1pr = press1 + pK.getRightDerivative() / 3.;
        x2pl = x2 - xNext.getLeftDerivative() / 3.;
        y2pl = y2 - yNext.getLeftDerivative() / 3.;
        press2pl = press2 - pNext.getLeftDerivative() / 3.;
        
        /*
         * Approximate the necessary number of line segments, using http://antigrain.com/research/adaptive_bezier/
         */
        double dx1,dy1,dx2,dy2,dx3,dy3;
        dx1 = x1pr - x1;
        dy1 = y1pr - y1;
        dx2 = x2pl - x1pr;
        dy2 = y2pl - y1pr;
        dx3 = x2 - x2pl;
        dy3 = y2 - y2pl;
        double length = std::sqrt(dx1 * dx1 + dy1 * dy1) +
        std::sqrt(dx2 * dx2 + dy2 * dy2) +
        std::sqrt(dx3 * dx3 + dy3 * dy3);
        double nbPointsPerSegment = (int)std::max(length * 0.25, 2.);
        
        double incr = 1. / (double)(nbPointsPerSegment - 1);
        
        int pot = 1 << mipMapLevel;
        for (double t = 0.; t <= 1.; t += incr) {
            
            Point p;
            p.x = bezier(x1, x1pr, x2pl, x2, t);
            p.y = bezier(y1, y1pr, y2pl, y2, t);
            double pi = bezier(press1, press1pr, press2pl, press2, t);
            p.x /= pot;
            p.y /= pot;
            points->push_back(std::make_pair(p, pi));
        }
    }
    
}
#endif

boost::shared_ptr<Double_Knob>
RotoStrokeItem::getBrushSizeKnob() const
{
    return _imp->brushSize;
}

boost::shared_ptr<Double_Knob>
RotoStrokeItem::getBrushHardnessKnob() const
{
    return _imp->brushHardness;
}

boost::shared_ptr<Double_Knob>
RotoStrokeItem::getBrushSpacingKnob() const
{
    return _imp->brushSpacing;
}

boost::shared_ptr<Double_Knob>
RotoStrokeItem::getBrushEffectKnob() const
{
    return _imp->effectStrength;
}

boost::shared_ptr<Double_Knob>
RotoStrokeItem::getBrushVisiblePortionKnob() const
{
    return _imp->visiblePortion;
}

////////////////////////////////////RotoContext////////////////////////////////////


RotoContext::RotoContext(const boost::shared_ptr<Natron::Node>& node)
    : _imp( new RotoContextPrivate(node) )
{
   
}

///Must be done here because at the time of the constructor, the shared_ptr doesn't exist yet but
///addLayer() needs it to get a shared ptr to this
void
RotoContext::createBaseLayer()
{
    ////Add the base layer
    boost::shared_ptr<RotoLayer> base = addLayerInternal(false);
    
    deselect(base, RotoItem::eSelectionReasonOther);
}

RotoContext::~RotoContext()
{
}

boost::shared_ptr<RotoLayer>
RotoContext::addLayerInternal(bool declarePython)
{
    boost::shared_ptr<RotoContext> this_shared = shared_from_this();
    assert(this_shared);
    
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    boost::shared_ptr<RotoLayer> item;
    
    std::string name = generateUniqueName(kRotoLayerBaseName);
    {
        
        boost::shared_ptr<RotoLayer> deepestLayer;
        boost::shared_ptr<RotoLayer> parentLayer;
        {
            QMutexLocker l(&_imp->rotoContextMutex);
            deepestLayer = findDeepestSelectedLayer();

            if (!deepestLayer) {
                ///find out if there's a base layer, if so add to the base layer,
                ///otherwise create the base layer
                for (std::list<boost::shared_ptr<RotoLayer> >::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
                    int hierarchy = (*it)->getHierarchyLevel();
                    if (hierarchy == 0) {
                        parentLayer = *it;
                        break;
                    }
                }
            } else {
                parentLayer = deepestLayer;
            }
        }
        
        item.reset( new RotoLayer(this_shared, name, boost::shared_ptr<RotoLayer>()) );
        if (parentLayer) {
            parentLayer->addItem(item,declarePython);
        }
        
        QMutexLocker l(&_imp->rotoContextMutex);

        _imp->layers.push_back(item);
        
        _imp->lastInsertedItem = item;
    }
    
    Q_EMIT itemInserted(RotoItem::eSelectionReasonOther);
    
    
    clearSelection(RotoItem::eSelectionReasonOther);
    select(item, RotoItem::eSelectionReasonOther);
    
    return item;

}

boost::shared_ptr<RotoLayer>
RotoContext::addLayer()
{
    return addLayerInternal(true);
} // addLayer

void
RotoContext::addLayer(const boost::shared_ptr<RotoLayer> & layer)
{
    std::list<boost::shared_ptr<RotoLayer> >::iterator it = std::find(_imp->layers.begin(), _imp->layers.end(), layer);

    if ( it == _imp->layers.end() ) {
        _imp->layers.push_back(layer);
    }
}

boost::shared_ptr<RotoItem>
RotoContext::getLastInsertedItem() const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->lastInsertedItem;
}

#ifdef NATRON_ROTO_INVERTIBLE
boost::shared_ptr<Bool_Knob>
RotoContext::getInvertedKnob() const
{
    return _imp->inverted.lock();
}

#endif

boost::shared_ptr<Color_Knob>
RotoContext::getColorKnob() const
{
    return _imp->colorKnob.lock();
}

void
RotoContext::setAutoKeyingEnabled(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->autoKeying = enabled;
}

bool
RotoContext::isAutoKeyingEnabled() const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->autoKeying;
}

void
RotoContext::setFeatherLinkEnabled(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->featherLink = enabled;
}

bool
RotoContext::isFeatherLinkEnabled() const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->featherLink;
}

void
RotoContext::setRippleEditEnabled(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->rippleEdit = enabled;
}

bool
RotoContext::isRippleEditEnabled() const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->rippleEdit;
}

boost::shared_ptr<Natron::Node>
RotoContext::getNode() const
{
    return _imp->node.lock();
}

int
RotoContext::getTimelineCurrentTime() const
{
    return getNode()->getApp()->getTimeLine()->currentFrame();
}

std::string
RotoContext::generateUniqueName(const std::string& baseName)
{
    int no = 1;
    
    bool foundItem;
    std::string name;
    do {
        std::stringstream ss;
        ss << baseName;
        ss << no;
        name = ss.str();
        if (getItemByName(name)) {
            foundItem = true;
        } else {
            foundItem = false;
        }
        ++no;
    } while (foundItem);
    return name;
}

boost::shared_ptr<Bezier>
RotoContext::makeBezier(double x,
                        double y,
                        const std::string & baseName,
                        int time)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    boost::shared_ptr<RotoLayer> parentLayer;
    boost::shared_ptr<RotoContext> this_shared = boost::dynamic_pointer_cast<RotoContext>(shared_from_this());
    assert(this_shared);
    std::string name = generateUniqueName(baseName);

    {

        QMutexLocker l(&_imp->rotoContextMutex);
        boost::shared_ptr<RotoLayer> deepestLayer = findDeepestSelectedLayer();


        if (!deepestLayer) {
            ///if there is no base layer, create one
            if ( _imp->layers.empty() ) {
                l.unlock();
                addLayer();
                l.relock();
            }
            parentLayer = _imp->layers.front();
        } else {
            parentLayer = deepestLayer;
        }
    }
    assert(parentLayer);
    boost::shared_ptr<Bezier> curve( new Bezier(this_shared, name, boost::shared_ptr<RotoLayer>()) );
    if (parentLayer) {
        parentLayer->addItem(curve);
    }
    _imp->lastInsertedItem = curve;

    Q_EMIT itemInserted(RotoItem::eSelectionReasonOther);


    clearSelection(RotoItem::eSelectionReasonOther);
    select(curve, RotoItem::eSelectionReasonOther);

    if ( isAutoKeyingEnabled() ) {
        curve->setKeyframe( getTimelineCurrentTime() );
    }
    curve->addControlPoint(x, y, time);
    
    return curve;
} // makeBezier

boost::shared_ptr<RotoStrokeItem>
RotoContext::makeStroke(Natron::RotoStrokeType type,const std::string& baseName)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    boost::shared_ptr<RotoLayer> parentLayer;
    boost::shared_ptr<RotoContext> this_shared = boost::dynamic_pointer_cast<RotoContext>(shared_from_this());
    assert(this_shared);
    std::string name = generateUniqueName(baseName);
    
    {
        
        QMutexLocker l(&_imp->rotoContextMutex);
        boost::shared_ptr<RotoLayer> deepestLayer = findDeepestSelectedLayer();
        
        
        if (!deepestLayer) {
            ///if there is no base layer, create one
            if ( _imp->layers.empty() ) {
                l.unlock();
                addLayer();
                l.relock();
            }
            parentLayer = _imp->layers.front();
        } else {
            parentLayer = deepestLayer;
        }
    }
    assert(parentLayer);
    boost::shared_ptr<RotoStrokeItem> curve( new RotoStrokeItem(type,this_shared, name, boost::shared_ptr<RotoLayer>()) );
    if (parentLayer) {
        parentLayer->addItem(curve);
    }
    _imp->lastInsertedItem = curve;
    
    Q_EMIT itemInserted(RotoItem::eSelectionReasonOther);
    
    
    clearSelection(RotoItem::eSelectionReasonOther);
    select(curve, RotoItem::eSelectionReasonOther);
    return curve;

}

boost::shared_ptr<Bezier>
RotoContext::makeEllipse(double x,double y,double diameter,bool fromCenter, int time)
{
    double half = diameter / 2.;
    boost::shared_ptr<Bezier> curve = makeBezier(x , fromCenter ? y - half : y ,kRotoEllipseBaseName, time);
    if (fromCenter) {
        curve->addControlPoint(x + half,y, time);
        curve->addControlPoint(x,y + half, time);
        curve->addControlPoint(x - half,y, time);
    } else {
        curve->addControlPoint(x + diameter,y - diameter, time);
        curve->addControlPoint(x,y - diameter, time);
        curve->addControlPoint(x - diameter,y - diameter, time);
    }
    
    boost::shared_ptr<BezierCP> top = curve->getControlPointAtIndex(0);
    boost::shared_ptr<BezierCP> right = curve->getControlPointAtIndex(1);
    boost::shared_ptr<BezierCP> bottom = curve->getControlPointAtIndex(2);
    boost::shared_ptr<BezierCP> left = curve->getControlPointAtIndex(3);

    double topX,topY,rightX,rightY,btmX,btmY,leftX,leftY;
    top->getPositionAtTime(time, &topX, &topY);
    right->getPositionAtTime(time, &rightX, &rightY);
    bottom->getPositionAtTime(time, &btmX, &btmY);
    left->getPositionAtTime(time, &leftX, &leftY);
    
    curve->setLeftBezierPoint(0, time,  (leftX + topX) / 2., topY);
    curve->setRightBezierPoint(0, time, (rightX + topX) / 2., topY);
    
    curve->setLeftBezierPoint(1, time,  rightX, (rightY + topY) / 2.);
    curve->setRightBezierPoint(1, time, rightX, (rightY + btmY) / 2.);
    
    curve->setLeftBezierPoint(2, time,  (rightX + btmX) / 2., btmY);
    curve->setRightBezierPoint(2, time, (leftX + btmX) / 2., btmY);
    
    curve->setLeftBezierPoint(3, time,   leftX, (btmY + leftY) / 2.);
    curve->setRightBezierPoint(3, time, leftX, (topY + leftY) / 2.);
    curve->setCurveFinished(true);
    
    return curve;
}

boost::shared_ptr<Bezier>
RotoContext::makeSquare(double x,double y,double initialSize,int time)
{
    boost::shared_ptr<Bezier> curve = makeBezier(x,y,kRotoRectangleBaseName,time);
    curve->addControlPoint(x + initialSize,y, time);
    curve->addControlPoint(x + initialSize,y - initialSize, time);
    curve->addControlPoint(x,y - initialSize, time);
    curve->setCurveFinished(true);
    
    return curve;

}

void
RotoContext::removeItemRecursively(const boost::shared_ptr<RotoItem>& item,
                                   RotoItem::SelectionReasonEnum reason)
{
    boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(item);
    boost::shared_ptr<RotoItem> foundSelected;

    for (std::list< boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        if (*it == item) {
            foundSelected = *it;
            break;
        }
    }
    if (foundSelected) {
        deselectInternal(foundSelected);
    }

    if (isLayer) {
        const RotoItems & items = isLayer->getItems();
        for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
            removeItemRecursively(*it, reason);
        }
        for (std::list<boost::shared_ptr<RotoLayer> >::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
            if (*it == isLayer) {
                _imp->layers.erase(it);
                break;
            }
        }
    }
    Q_EMIT itemRemoved(item,(int)reason);
}

void
RotoContext::removeItem(const boost::shared_ptr<RotoItem>& item,
                        RotoItem::SelectionReasonEnum reason)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        boost::shared_ptr<RotoLayer> layer = item->getParentLayer();
        if (layer) {
            layer->removeItem(item);
        }
        removeItemRecursively(item,reason);
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::addItem(const boost::shared_ptr<RotoLayer>& layer,
                     int indexInLayer,
                     const boost::shared_ptr<RotoItem> & item,
                     RotoItem::SelectionReasonEnum reason)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    {
        if (layer) {
            layer->insertItem(item,indexInLayer);
        }
        
        QMutexLocker l(&_imp->rotoContextMutex);

        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(item);
        if (isLayer) {
            std::list<boost::shared_ptr<RotoLayer> >::iterator foundLayer = std::find(_imp->layers.begin(), _imp->layers.end(), isLayer);
            if ( foundLayer == _imp->layers.end() ) {
                _imp->layers.push_back(isLayer);
            }
        }
        _imp->lastInsertedItem = item;
    }
    Q_EMIT itemInserted(reason);
}


const std::list< boost::shared_ptr<RotoLayer> > &
RotoContext::getLayers() const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->layers;
}

boost::shared_ptr<Bezier>
RotoContext::isNearbyBezier(double x,
                            double y,
                            double acceptance,
                            int* index,
                            double* t,
                            bool* feather) const
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    QMutexLocker l(&_imp->rotoContextMutex);
    for (std::list< boost::shared_ptr<RotoLayer> >::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        const RotoItems & items = (*it)->getItems();
        for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            boost::shared_ptr<Bezier> b = boost::dynamic_pointer_cast<Bezier>(*it2);
            if (b && !b->isLockedRecursive()) {
                double param;
                int i = b->isPointOnCurve(x, y, acceptance, &param,feather);
                if (i != -1) {
                    *index = i;
                    *t = param;

                    return b;
                }
            }
        }
    }

    return boost::shared_ptr<Bezier>();
}

void
RotoContext::onAutoKeyingChanged(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->autoKeying = enabled;
}

void
RotoContext::onFeatherLinkChanged(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->featherLink = enabled;
}

void
RotoContext::onRippleEditChanged(bool enabled)
{
    ///MT-safe: only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->rippleEdit = enabled;
}

void
RotoContext::getMaskRegionOfDefinition(int time,
                                       int /*view*/,
                                       RectD* rod) // rod is in canonical coordinates
const
{
    QMutexLocker l(&_imp->rotoContextMutex);
    bool first = true;

    for (std::list<boost::shared_ptr<RotoLayer> >::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        RotoItems items = (*it)->getItems_mt_safe();
        for (RotoItems::iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it2->get());
            RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it2->get());
            if (isBezier && !isStroke) {
                if (isBezier->isActivated(time) && isBezier->isCurveFinished() && isBezier->getControlPointsCount() > 1) {
                    RectD splineRoD = isBezier->getBoundingBox(time);
                    if ( splineRoD.isNull() ) {
                        continue;
                    }
                    
                    if (first) {
                        first = false;
                        *rod = splineRoD;
                    } else {
                        rod->merge(splineRoD);
                    }
                }
            } else if (isStroke) {
                RectD strokeRod = isStroke->getBoundingBox(time);
                if (first) {
                    first = false;
                    *rod = strokeRod;
                } else {
                    rod->merge(strokeRod);
                }
            }
        }
    }
}

bool
RotoContext::isEmpty() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->layers.empty();
}

void
RotoContext::save(RotoContextSerialization* obj) const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    obj->_autoKeying = _imp->autoKeying;
    obj->_featherLink = _imp->featherLink;
    obj->_rippleEdit = _imp->rippleEdit;

    ///There must always be the base layer
    assert( !_imp->layers.empty() );

    ///Serializing this layer will recursively serialize everything
    _imp->layers.front()->save( dynamic_cast<RotoItemSerialization*>(&obj->_baseLayer) );

    ///the age of the context is not serialized as the images are wiped from the cache anyway

    ///Serialize the selection
    for (std::list<boost::shared_ptr<RotoItem> >::const_iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        obj->_selectedItems.push_back( (*it)->getScriptName() );
    }
}

static void
linkItemsKnobsRecursively(RotoContext* ctx,
                          const boost::shared_ptr<RotoLayer> & layer)
{
    const RotoItems & items = layer->getItems();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);

        if (isBezier) {
            ctx->select(isBezier, RotoItem::eSelectionReasonOther);
        } else if (isLayer) {
            linkItemsKnobsRecursively(ctx, isLayer);
        }
    }
}

void
RotoContext::load(const RotoContextSerialization & obj)
{
    assert( QThread::currentThread() == qApp->thread() );
    ///no need to lock here, when this is called the main-thread is the only active thread

    _imp->autoKeying = obj._autoKeying;
    _imp->featherLink = obj._featherLink;
    _imp->rippleEdit = obj._rippleEdit;

    for (std::list<boost::weak_ptr<KnobI> >::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
        it->lock()->setAllDimensionsEnabled(false);
    }

    assert(_imp->layers.size() == 1);

    boost::shared_ptr<RotoLayer> baseLayer = _imp->layers.front();

    baseLayer->load(obj._baseLayer);

    for (std::list<std::string>::const_iterator it = obj._selectedItems.begin(); it != obj._selectedItems.end(); ++it) {
        boost::shared_ptr<RotoItem> item = getItemByName(*it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(item);
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(item);
        if (isBezier) {
            select(isBezier,RotoItem::eSelectionReasonOther);
        } else if (isLayer) {
            linkItemsKnobsRecursively(this, isLayer);
        }
    }
}

void
RotoContext::select(const boost::shared_ptr<RotoItem> & b,
                    RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        selectInternal(b);
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::select(const std::list<boost::shared_ptr<RotoDrawableItem> > & beziers,
                    RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoDrawableItem> >::const_iterator it = beziers.begin(); it != beziers.end(); ++it) {
            selectInternal(*it);
        }
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::select(const std::list<boost::shared_ptr<RotoItem> > & items,
                    RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoItem> >::const_iterator it = items.begin(); it != items.end(); ++it) {
            selectInternal(*it);
        }
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::deselect(const boost::shared_ptr<RotoItem> & b,
                      RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        deselectInternal(b);
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::deselect(const std::list<boost::shared_ptr<Bezier> > & beziers,
                      RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<Bezier> >::const_iterator it = beziers.begin(); it != beziers.end(); ++it) {
            deselectInternal(*it);
        }
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::deselect(const std::list<boost::shared_ptr<RotoItem> > & items,
                      RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoItem> >::const_iterator it = items.begin(); it != items.end(); ++it) {
            deselectInternal(*it);
        }
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::clearSelection(RotoItem::SelectionReasonEnum reason)
{
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        while ( !_imp->selectedItems.empty() ) {
            deselectInternal( _imp->selectedItems.front() );
        }
    }
    Q_EMIT selectionChanged( (int)reason );
}

void
RotoContext::selectInternal(const boost::shared_ptr<RotoItem> & item)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    assert( !_imp->rotoContextMutex.tryLock() );

    int nbUnlockedBeziers = 0;
    int nbUnlockedStrokes = 0;
    bool foundItem = false;
    for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
        if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
            ++nbUnlockedBeziers;
        } else if (isStroke) {
            ++nbUnlockedStrokes;
        }
        if (it->get() == item.get()) {
            foundItem = true;
        }
    }

    ///the item is already selected, exit
    if (foundItem) {
        return;
    }


    boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(item);
    boost::shared_ptr<RotoStrokeItem> isStroke = boost::dynamic_pointer_cast<RotoStrokeItem>(item);
    RotoDrawableItem* isDrawable = dynamic_cast<RotoDrawableItem*>(item.get());
    boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(item);

    if (isDrawable) {
        if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
            ++nbUnlockedBeziers;
        } else if (isStroke) {
            ++nbUnlockedStrokes;
        }
        
        const std::list<boost::shared_ptr<KnobI> >& drawableKnobs = isDrawable->getKnobs();
        for (std::list<boost::shared_ptr<KnobI> >::const_iterator it = drawableKnobs.begin(); it != drawableKnobs.end(); ++it) {
            
            for (std::list<boost::weak_ptr<KnobI> >::iterator it2 = _imp->knobs.begin(); it2 != _imp->knobs.end(); ++it2) {
                boost::shared_ptr<KnobI> thisKnob = it2->lock();
                if (thisKnob->getName() == (*it)->getName()) {
                    
                    //Clone current state
                    thisKnob->cloneAndUpdateGui(it->get());
                    
                    //Slave internal knobs of the bezier
                    assert((*it)->getDimension() == thisKnob->getDimension());
                    for (int i = 0; i < (*it)->getDimension(); ++i) {
                        (*it)->slaveTo(i, thisKnob, i);
                    }
                    
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(SequenceTime,int,int,bool)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(SequenceTime,int,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(int,int,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(SequenceTime,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));

                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(SequenceTime,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));

                    break;
                }
            }
            
        }

    } else if (isLayer) {
        const RotoItems & children = isLayer->getItems();
        for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
            selectInternal(*it);
        }
    }
    
    ///enable the knobs
    if (nbUnlockedBeziers > 0 || nbUnlockedStrokes > 0) {
        for (std::list<boost::weak_ptr<KnobI> >::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            boost::shared_ptr<KnobI> k = it->lock();
            if (!k) {
                continue;
            }
            k->setAllDimensionsEnabled(true);
            if (nbUnlockedBeziers >= 2 || nbUnlockedStrokes >= 2) {
                k->setDirty(true);
            }
        }
    }
    _imp->selectedItems.push_back(item);
} // selectInternal

void
RotoContext::onSelectedKnobCurveChanged()
{
    KnobSignalSlotHandler* handler = qobject_cast<KnobSignalSlotHandler*>(sender());
    if (handler) {
        boost::shared_ptr<KnobI> knob = handler->getKnob();
        for (std::list<boost::weak_ptr<KnobI> >::const_iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            boost::shared_ptr<KnobI> k = it->lock();
            if (k->getName() == knob->getName()) {
                k->clone(knob.get());
                break;
            }
        }
    }
}

void
RotoContext::deselectInternal(boost::shared_ptr<RotoItem> b)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    assert( !_imp->rotoContextMutex.tryLock() );

    std::list<boost::shared_ptr<RotoItem> >::iterator foundSelected = std::find(_imp->selectedItems.begin(),_imp->selectedItems.end(),b);

    ///if the item is not selected, exit
    if ( foundSelected == _imp->selectedItems.end() ) {
        return;
    }

    _imp->selectedItems.erase(foundSelected);

    int nbBeziersUnLockedBezier = 0;
    int nbStrokesUnlocked = 0;
    for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
        if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
            ++nbBeziersUnLockedBezier;
        } else if (isStroke) {
            ++nbStrokesUnlocked;
        }
    }
    bool bezierDirty = nbBeziersUnLockedBezier > 1;
    bool strokeDirty = nbStrokesUnlocked > 1;
    boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(b);
    RotoDrawableItem* isDrawable = dynamic_cast<RotoDrawableItem*>(b.get());
    boost::shared_ptr<RotoStrokeItem> isStroke = boost::dynamic_pointer_cast<RotoStrokeItem>(b);
    boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(b);
    if (isDrawable) {
        ///first-off set the context knobs to the value of this bezier
        
        const std::list<boost::shared_ptr<KnobI> >& drawableKnobs = isDrawable->getKnobs();
        for (std::list<boost::shared_ptr<KnobI> >::const_iterator it = drawableKnobs.begin(); it != drawableKnobs.end(); ++it) {
            
            for (std::list<boost::weak_ptr<KnobI> >::iterator it2 = _imp->knobs.begin(); it2 != _imp->knobs.end(); ++it2) {
                boost::shared_ptr<KnobI> knob = it2->lock();
                if (knob->getName() == (*it)->getName()) {
                    
                    //Clone current state
                    knob->cloneAndUpdateGui(it->get());
                    
                    //Slave internal knobs of the bezier
                    assert((*it)->getDimension() == knob->getDimension());
                    for (int i = 0; i < (*it)->getDimension(); ++i) {
                        (*it)->unSlave(i,isBezier ? !bezierDirty : !strokeDirty);
                    }
                    
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(SequenceTime,int,int,bool)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(SequenceTime,int,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(int,int,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(SequenceTime,int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(SequenceTime,int)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    break;
                }
            }
            
        }
        
        
    } else if (isLayer) {
        const RotoItems & children = isLayer->getItems();
        for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
            deselectInternal(*it);
        }
    }
    
    
    
    ///if the selected beziers count reaches 0 notify the gui knobs so they appear not enabled
    
    if (nbBeziersUnLockedBezier == 0 || nbStrokesUnlocked == 0) {
        for (std::list<boost::weak_ptr<KnobI> >::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            boost::shared_ptr<KnobI> k = it->lock();
            if (!k) {
                continue;
            }
            k->setAllDimensionsEnabled(false);
            if (!bezierDirty || !strokeDirty) {
                k->setDirty(false);
            }
        }
    }
    
} // deselectInternal


boost::shared_ptr<RotoItem>
RotoContext::getLastItemLocked() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->lastLockedItem;
}

static void
addOrRemoveKeyRecursively(const boost::shared_ptr<RotoLayer>& isLayer,
                          int time,
                          bool add,
                          bool removeAll)
{
    const RotoItems & items = isLayer->getItems();

    for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
        boost::shared_ptr<RotoLayer> layer = boost::dynamic_pointer_cast<RotoLayer>(*it2);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it2);
        if (isBezier) {
            if (add) {
                isBezier->setKeyframe(time);
            } else {
                if (!removeAll) {
                    isBezier->removeKeyframe(time);
                } else {
                    isBezier->removeAnimation();
                }
            }
        } else if (layer) {
            addOrRemoveKeyRecursively(layer, time, add,removeAll);
        }
    }
}

void
RotoContext::setKeyframeOnSelectedCurves()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    int time = getTimelineCurrentTime();
    QMutexLocker l(&_imp->rotoContextMutex);
    for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->setKeyframe(time);
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer,time, true, false);
        }
    }
}

void
RotoContext::removeAnimationOnSelectedCurves()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    
    int time = getTimelineCurrentTime();
    for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->removeAnimation();
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer,time, false, true);
        }
    }
    if (!_imp->selectedItems.empty()) {
        evaluateChange();
    }
}

void
RotoContext::removeKeyframeOnSelectedCurves()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    int time = getTimelineCurrentTime();
    for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->removeKeyframe(time);
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer,time, false, false);
        }
    }
}

static void
findOutNearestKeyframeRecursively(const boost::shared_ptr<RotoLayer>& layer,
                                  bool previous,
                                  int time,
                                  int* nearest)
{
    const RotoItems & items = layer->getItems();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        boost::shared_ptr<RotoLayer> layer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            if (previous) {
                int t = isBezier->getPreviousKeyframeTime(time);
                if ( (t != INT_MIN) && (t > *nearest) ) {
                    *nearest = t;
                }
            } else if (layer) {
                int t = isBezier->getNextKeyframeTime(time);
                if ( (t != INT_MAX) && (t < *nearest) ) {
                    *nearest = t;
                }
            }
        } else {
            assert(layer);
            if (layer) {
                findOutNearestKeyframeRecursively(layer, previous, time, nearest);
            }
        }
    }
}

void
RotoContext::goToPreviousKeyframe()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    int time = getTimelineCurrentTime();
    int minimum = INT_MIN;

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            boost::shared_ptr<RotoLayer> layer = boost::dynamic_pointer_cast<RotoLayer>(*it);
            boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
            if (isBezier) {
                int t = isBezier->getPreviousKeyframeTime(time);
                if ( (t != INT_MIN) && (t > minimum) ) {
                    minimum = t;
                }
            } else {
                assert(layer);
                if (layer) {
                    findOutNearestKeyframeRecursively(layer, true,time,&minimum);
                }
            }
        }
    }

    if (minimum != INT_MIN) {
        getNode()->getApp()->setLastViewerUsingTimeline(boost::shared_ptr<Natron::Node>());
        getNode()->getApp()->getTimeLine()->seekFrame(minimum, false,  NULL, Natron::eTimelineChangeReasonPlaybackSeek);
    }
}

void
RotoContext::goToNextKeyframe()
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    int time = getTimelineCurrentTime();
    int maximum = INT_MAX;

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
            boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
            if (isBezier) {
                int t = isBezier->getNextKeyframeTime(time);
                if ( (t != INT_MAX) && (t < maximum) ) {
                    maximum = t;
                }
            } else {
                assert(isLayer);
                if (isLayer) {
                    findOutNearestKeyframeRecursively(isLayer, false, time, &maximum);
                }
            }
        }
    }
    if (maximum != INT_MAX) {
        getNode()->getApp()->setLastViewerUsingTimeline(boost::shared_ptr<Natron::Node>());
        getNode()->getApp()->getTimeLine()->seekFrame(maximum, false, NULL,Natron::eTimelineChangeReasonPlaybackSeek);
        
    }
}

static void
appendToSelectedCurvesRecursively(std::list< boost::shared_ptr<RotoDrawableItem> > * curves,
                                  const boost::shared_ptr<RotoLayer>& isLayer,
                                  int time,
                                  bool onlyActives,
                                  bool addStrokes)
{
    RotoItems items = isLayer->getItems_mt_safe();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        boost::shared_ptr<RotoLayer> layer = boost::dynamic_pointer_cast<RotoLayer>(*it);
        boost::shared_ptr<RotoDrawableItem> isDrawable = boost::dynamic_pointer_cast<RotoDrawableItem>(*it);
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(isDrawable.get());
        if (isStroke && !addStrokes) {
            continue;
        }
        if (isDrawable) {
            if ( !onlyActives || isDrawable->isActivated(time) ) {
                curves->push_back(isDrawable);
            }
        } else if ( layer && layer->isGloballyActivated() ) {
            appendToSelectedCurvesRecursively(curves, layer, time, onlyActives ,addStrokes);
        }
    }
}

const std::list< boost::shared_ptr<RotoItem> > &
RotoContext::getSelectedItems() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->selectedItems;
}

std::list< boost::shared_ptr<RotoDrawableItem> >
RotoContext::getSelectedCurves() const
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    std::list< boost::shared_ptr<RotoDrawableItem> > drawables;
    int time = getTimelineCurrentTime();
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            assert(*it);
            boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
            boost::shared_ptr<RotoDrawableItem> isDrawable = boost::dynamic_pointer_cast<RotoDrawableItem>(*it);
            if (isDrawable) {
                drawables.push_back(isDrawable);
            } else {
                assert(isLayer);
                if (isLayer) {
                    appendToSelectedCurvesRecursively(&drawables, isLayer,time,false, true);
                }
            }
        }
    }
    return drawables;
}

std::list< boost::shared_ptr<RotoDrawableItem> >
RotoContext::getCurvesByRenderOrder() const
{
    std::list< boost::shared_ptr<RotoDrawableItem> > ret;
    
    ///Note this might not be the timeline's current frame if this is a render thread.
    int time = getNode()->getLiveInstance()->getThreadLocalRenderTime();
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        if ( !_imp->layers.empty() ) {
            appendToSelectedCurvesRecursively(&ret, _imp->layers.front(), time, true, true);
        }
    }

    return ret;
}

int
RotoContext::getNCurves() const
{
    std::list< boost::shared_ptr<RotoDrawableItem> > curves = getCurvesByRenderOrder();
    return (int)curves.size();
}

boost::shared_ptr<RotoLayer>
RotoContext::getLayerByName(const std::string & n) const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    for (std::list<boost::shared_ptr<RotoLayer> >::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        if ( (*it)->getScriptName() == n ) {
            return *it;
        }
    }

    return boost::shared_ptr<RotoLayer>();
}

static void
findItemRecursively(const std::string & n,
                    const boost::shared_ptr<RotoLayer> & layer,
                    boost::shared_ptr<RotoItem>* ret)
{
    if (layer->getScriptName() == n) {
        *ret = boost::dynamic_pointer_cast<RotoItem>(layer);
    } else {
        const RotoItems & items = layer->getItems();
        for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it2);
            if ( (*it2)->getScriptName() == n ) {
                *ret = *it2;

                return;
            } else if (isLayer) {
                findItemRecursively(n, isLayer, ret);
            }
        }
    }
}

boost::shared_ptr<RotoItem>
RotoContext::getItemByName(const std::string & n) const
{
    boost::shared_ptr<RotoItem> ret;
    QMutexLocker l(&_imp->rotoContextMutex);

    for (std::list<boost::shared_ptr<RotoLayer> >::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        findItemRecursively(n, *it, &ret);
    }

    return ret;
}

boost::shared_ptr<RotoLayer>
RotoContext::getDeepestSelectedLayer() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return findDeepestSelectedLayer();
}

boost::shared_ptr<RotoLayer>
RotoContext::findDeepestSelectedLayer() const
{
    assert( !_imp->rotoContextMutex.tryLock() );

    int minLevel = -1;
    boost::shared_ptr<RotoLayer> minLayer;
    for (std::list< boost::shared_ptr<RotoItem> >::const_iterator it = _imp->selectedItems.begin();
         it != _imp->selectedItems.end(); ++it) {
        int lvl = (*it)->getHierarchyLevel();
        if (lvl > minLevel) {
            boost::shared_ptr<RotoLayer> isLayer = boost::dynamic_pointer_cast<RotoLayer>(*it);
            if (isLayer) {
                minLayer = isLayer;
            } else {
                minLayer = (*it)->getParentLayer();
            }
            minLevel = lvl;
        }
    }

    return minLayer;
}

void
RotoContext::evaluateChange()
{
    _imp->incrementRotoAge();
    getNode()->getLiveInstance()->evaluate_public(NULL, true,Natron::eValueChangedReasonUserEdited);
}

U64
RotoContext::getAge()
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->age;
}

void
RotoContext::onItemLockedChanged(const boost::shared_ptr<RotoItem>& item, RotoItem::SelectionReasonEnum reason)
{
    assert(item);
    ///refresh knobs
    int nbBeziersUnLockedBezier = 0;

    {
        QMutexLocker l(&_imp->rotoContextMutex);

        for (std::list<boost::shared_ptr<RotoItem> >::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            boost::shared_ptr<Bezier> isBezier = boost::dynamic_pointer_cast<Bezier>(*it);
            if ( isBezier && !isBezier->isLockedRecursive() ) {
                ++nbBeziersUnLockedBezier;
            }
        }
    }
    bool dirty = nbBeziersUnLockedBezier > 1;
    bool enabled = nbBeziersUnLockedBezier > 0;

    _imp->activated.lock()->setDirty(dirty);
    _imp->opacity.lock()->setDirty(dirty);
    _imp->feather.lock()->setDirty(dirty);
    _imp->featherFallOff.lock()->setDirty(dirty);
#ifdef NATRON_ROTO_INVERTIBLE
    _imp->inverted.lock()->setDirty(dirty);
#endif

    _imp->activated.lock()->setAllDimensionsEnabled(enabled);
    _imp->opacity.lock()->setAllDimensionsEnabled(enabled);
    _imp->featherFallOff.lock()->setAllDimensionsEnabled(enabled);
    _imp->feather.lock()->setAllDimensionsEnabled(enabled);
#ifdef NATRON_ROTO_INVERTIBLE
    _imp->inverted.lock()->setAllDimensionsEnabled(enabled);
#endif
    _imp->lastLockedItem = item;
    Q_EMIT itemLockedChanged((int)reason);
}

void
RotoContext::onItemScriptNameChanged(const boost::shared_ptr<RotoItem>& item)
{
    Q_EMIT itemScriptNameChanged(item);
}

void
RotoContext::onItemLabelChanged(const boost::shared_ptr<RotoItem>& item)
{
    Q_EMIT itemLabelChanged(item);
}

void
RotoContext::onItemKnobChanged()
{
    emitRefreshViewerOverlays();
    evaluateChange();
}

std::string
RotoContext::getRotoNodeName() const
{
    return getNode()->getScriptName_mt_safe();
}

void
RotoContext::emitRefreshViewerOverlays()
{
    Q_EMIT refreshViewerOverlays();
}

void
RotoContext::getBeziersKeyframeTimes(std::list<int> *times) const
{
    std::list< boost::shared_ptr<RotoDrawableItem> > splines = getCurvesByRenderOrder();

    for (std::list< boost::shared_ptr<RotoDrawableItem> > ::iterator it = splines.begin(); it != splines.end(); ++it) {
        std::set<int> splineKeys;
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        if (!isBezier) {
            continue;
        }
        isBezier->getKeyframeTimes(&splineKeys);
        for (std::set<int>::iterator it2 = splineKeys.begin(); it2 != splineKeys.end(); ++it2) {
            times->push_back(*it2);
        }
    }
}

static void
adjustToPointToScale(unsigned int mipmapLevel,
                     double &x,
                     double &y)
{
    if (mipmapLevel != 0) {
        int pot = (1 << mipmapLevel);
        x /= pot;
        y /= pot;
    }
}

template <typename PIX,int maxValue,int srcNComps, int dstNComps>
static void
convertCairoImageToNatronImageForDstComponents(cairo_surface_t* cairoImg,
                                               Natron::Image* image,
                                               const RectI & pixelRod)
{
    
    unsigned char* cdata = cairo_image_surface_get_data(cairoImg);
    unsigned char* srcPix = cdata;
    int stride = cairo_image_surface_get_stride(cairoImg);
    int pixelSize = stride / pixelRod.width();
    
    Natron::Image::WriteAccess acc = image->getWriteRights();
    
    for (int y = 0; y < pixelRod.height(); ++y, srcPix += stride) {
        PIX* dstPix = (PIX*)acc.pixelAt(pixelRod.x1, pixelRod.y1 + y);
        assert(dstPix);
        
        for (int x = 0; x < pixelRod.width(); ++x) {
            switch (dstNComps) {
                case 4:
                    assert(srcNComps == dstNComps);
                    // cairo's format is ARGB (that is BGRA when interpreted as bytes)
                    dstPix[x * dstNComps + 3] = PIX( (float)srcPix[x * pixelSize + 3] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 0] = PIX( (float)srcPix[x * pixelSize + 2] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 1] = PIX( (float)srcPix[x * pixelSize + 1] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 2] = PIX( (float)srcPix[x * pixelSize + 0] / 255.f ) * maxValue;
                    break;
                case 1:
                    assert(srcNComps == dstNComps);
                    dstPix[x] = PIX( (float)srcPix[x] / 255.f ) * maxValue;
                    break;
                case 3:
                    assert(srcNComps == dstNComps);
                    dstPix[x * dstNComps + 0] = PIX( (float)srcPix[x * pixelSize + 2] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 1] = PIX( (float)srcPix[x * pixelSize + 1] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 2] = PIX( (float)srcPix[x * pixelSize + 0] / 255.f ) * maxValue;
                    break;
                case 2:
                    assert(srcNComps == 3);
                    dstPix[x * dstNComps + 0] = PIX( (float)srcPix[x * pixelSize + 2] / 255.f ) * maxValue;
                    dstPix[x * dstNComps + 1] = PIX( (float)srcPix[x * pixelSize + 1] / 255.f ) * maxValue;
                    break;

                default:
                    break;
            }
            
            
        }
    }

}

template <typename PIX,int maxValue,int srcNComps>
static void
convertCairoImageToNatronImageForSrcComponents(cairo_surface_t* cairoImg,
                               Natron::Image* image,
                               const RectI & pixelRod)
{
    int comps = (int)image->getComponentsCount();
    switch (comps) {
        case 1:
            convertCairoImageToNatronImageForDstComponents<PIX,maxValue,srcNComps,1>(cairoImg,image,pixelRod);
            break;
        case 2:
            convertCairoImageToNatronImageForDstComponents<PIX,maxValue,srcNComps,2>(cairoImg,image,pixelRod);
            break;
        case 3:
            convertCairoImageToNatronImageForDstComponents<PIX,maxValue,srcNComps,3>(cairoImg,image,pixelRod);
            break;
        case 4:
            convertCairoImageToNatronImageForDstComponents<PIX,maxValue,srcNComps,4>(cairoImg,image,pixelRod);
            break;
        default:
            break;
    }
    
}

template <typename PIX,int maxValue>
static void
convertCairoImageToNatronImage(cairo_surface_t* cairoImg,
                               Natron::Image* image,
                               const RectI & pixelRod,
                               int srcNComps)
{
    switch (srcNComps) {
        case 1:
            convertCairoImageToNatronImageForSrcComponents<PIX, maxValue, 1>(cairoImg, image, pixelRod);
            break;
        case 2:
            convertCairoImageToNatronImageForSrcComponents<PIX, maxValue, 2>(cairoImg, image, pixelRod);
            break;
        case 3:
            convertCairoImageToNatronImageForSrcComponents<PIX, maxValue, 3>(cairoImg, image, pixelRod);
            break;
        case 4:
            convertCairoImageToNatronImageForSrcComponents<PIX, maxValue, 4>(cairoImg, image, pixelRod);
            break;
            
        default:
            break;
    }
}

boost::shared_ptr<Natron::Image>
RotoContext::renderMask(const RectI & roi,
                        const Natron::ImageComponents& components,
                        U64 nodeHash,
                        U64 ageToRender,
                        const RectD & nodeRoD, //!< rod in canonical coordinates
                        SequenceTime time,
                        Natron::ImageBitDepthEnum depth,
                        int view,
                        unsigned int mipmapLevel)
{
    std::list< boost::shared_ptr<RotoDrawableItem> > splines = getCurvesByRenderOrder();

    ///compute an enhanced hash different from the one of the node in order to differentiate within the cache
    ///the output image of the roto node and the mask image.
    Hash64 hash;

    hash.append(nodeHash);
    hash.append(ageToRender);
    hash.computeHash();

    Natron::ImageKey key = Natron::Image::makeKey(hash.value(), true ,time, view);

    ///If the last rendered image  was with a different hash key (i.e a parameter changed or an input changed)
    ///just remove the old image from the cache to recycle memory.
    boost::shared_ptr<Image> lastRenderedImage;
    U64 lastRenderHash;
    {
        QMutexLocker l(&_imp->lastRenderArgsMutex);
        lastRenderHash = _imp->lastRenderHash;
        lastRenderedImage = _imp->lastRenderedImage;
    }

    if ( lastRenderedImage &&
         ( lastRenderHash != hash.value() ) ) {
        
        appPTR->removeAllImagesFromCacheWithMatchingKey(lastRenderHash);

        {
            QMutexLocker l(&_imp->lastRenderArgsMutex);
            _imp->lastRenderedImage.reset();
        }
    }

    boost::shared_ptr<Node> node = getNode();
    
    boost::shared_ptr<Natron::ImageParams> params;
    ImagePtr image;
    
//    if (!byPassCache) {
//
//        getNode()->getLiveInstance()->getImageFromCacheAndConvertIfNeeded(useCache, false,  key, mipmapLevel,
//                                                                          roi,
//                                                                          nodeRoD,
//                                                                          depth, components,
//                                                                           depth, components,inputImages,&image);
//        if (image) {
//            params = image->getParams();
//        }
//    }
    
    ///If there's only 1 shape to render and this shape is inverted, initialize the image
    ///with the invert instead of the default fill value to speed up rendering
    if (!image) {
        
        params = Natron::Image::makeParams( 0,
                                           nodeRoD,
                                           roi,
                                           1., // par
                                           mipmapLevel,
                                           false,
                                           components,
                                           depth,
                                           std::map<int,std::map<int, std::vector<RangeD> > >() );
        
        Natron::getImageFromCacheOrCreate(key, params, &image);
        if (!image) {
            std::stringstream ss;
            ss << "Failed to allocate an image of ";
            ss << printAsRAM( params->getElementsCount() * sizeof(Natron::Image::data_t) ).toStdString();
            Natron::errorDialog( QObject::tr("Out of memory").toStdString(),ss.str() );
            
            return image;
        }
        
        ///Does nothing if image is already alloc
        image->allocateMemory();
        
        /*
         * Another thread might have allocated the same image in the cache but with another RoI, make sure
         * it is big enough for us, or resize it to our needs.
         */
        
        image->ensureBounds(params->getBounds());
    
        
    }

    
    ///////////////////////////////////Render internal
    RectI pixelRod = params->getBounds();
    RectI clippedRoI;
    roi.intersect(pixelRod, &clippedRoI);

    cairo_format_t cairoImgFormat;
    
    int srcNComps;
    if (components.getNumComponents() == 1) {
        cairoImgFormat = CAIRO_FORMAT_A8;
        srcNComps = 1;
    } else if (components.getNumComponents() == 2) {
        cairoImgFormat = CAIRO_FORMAT_RGB24;
        srcNComps = 3;
    } else if (components.getNumComponents() == 3) {
        cairoImgFormat = CAIRO_FORMAT_RGB24;
        srcNComps = 3;
    } else if (components.getNumComponents() == 4) {
        cairoImgFormat = CAIRO_FORMAT_ARGB32;
        srcNComps = 4;
    } else {
        cairoImgFormat = CAIRO_FORMAT_A8;
        srcNComps = 1;
    }

    ////Allocate the cairo temporary buffer
    cairo_surface_t* cairoImg = cairo_image_surface_create(cairoImgFormat, pixelRod.width(), pixelRod.height() );
    cairo_surface_set_device_offset(cairoImg, -pixelRod.x1, -pixelRod.y1);
    if (cairo_surface_status(cairoImg) != CAIRO_STATUS_SUCCESS) {
        appPTR->removeFromNodeCache(image);

        return image;
    }
    cairo_t* cr = cairo_create(cairoImg);
    //cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); // creates holes on self-overlapping shapes
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

    ///We could also propose the user to render a mask to SVG
    _imp->renderInternal(cr, cairoImg, splines,mipmapLevel,time);

    switch (depth) {
    case Natron::eImageBitDepthFloat:
        convertCairoImageToNatronImage<float, 1>(cairoImg, image.get(), pixelRod,srcNComps);
        break;
    case Natron::eImageBitDepthByte:
        convertCairoImageToNatronImage<unsigned char, 255>(cairoImg, image.get(), pixelRod,srcNComps);
        break;
    case Natron::eImageBitDepthShort:
        convertCairoImageToNatronImage<unsigned short, 65535>(cairoImg, image.get(), pixelRod,srcNComps);
        break;
    case Natron::eImageBitDepthNone:
        assert(false);
        break;
    }

    cairo_destroy(cr);
    ////Free the buffer used by Cairo
    cairo_surface_destroy(cairoImg);


    ////////////////////////////////////
    if ( node->aborted() ) {
        //if render was aborted, remove the frame from the cache as it contains only garbage
        appPTR->removeFromNodeCache(image);
    } else {
        image->markForRendered(clippedRoI);
    }

    {
        QMutexLocker l(&_imp->lastRenderArgsMutex);
        _imp->lastRenderHash = hash.value();
        _imp->lastRenderedImage = image;
    }

    return image;
} // renderMask

void
RotoContextPrivate::renderInternal(cairo_t* cr,
                                   cairo_surface_t* cairoImg,
                                   const std::list< boost::shared_ptr<RotoDrawableItem> > & splines,
                                   unsigned int mipmapLevel,
                                   int time)
{
    // these Roto shapes must be rendered WITHOUT antialias, or the junction between the inner
    // polygon and the feather zone will have artifacts. This is partly due to the fact that cairo
    // meshes are not antialiased.
    // Use a default feather distance of 1 pixel instead!
    // UPDATE: unfortunately, this produces less artifacts, but there are still some remaining (use opacity=0.5 to test)
    // maybe the inner polygon should be made of mesh patterns too?
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    for (std::list<boost::shared_ptr<RotoDrawableItem> >::const_iterator it2 = splines.begin(); it2 != splines.end(); ++it2) {
        
        Bezier* isBezier = dynamic_cast<Bezier*>(it2->get());
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it2->get());
        if (isBezier && !isStroke) {
            renderBezier(cr, isBezier, time, mipmapLevel);
        } else if (isStroke) {
            renderStroke(cr, isStroke, time, mipmapLevel);
        }
        
        
    } // foreach(splines)
    assert(cairo_surface_status(cairoImg) == CAIRO_STATUS_SUCCESS);

    ///A call to cairo_surface_flush() is required before accessing the pixel data
    ///to ensure that all pending drawing operations are finished.
    cairo_surface_flush(cairoImg);
} // renderInternal

/**
 * @brief Renders 1 patch of a paint stroke's dot. Each dot is separated in 4 patches (tOp left, top right, bottom left, bottom right).
 * Think of a circle divided in 4 equal parts.
 * patchNum = 0 = topRight
 *          = 1 = topLeft
 *          = 2 = bottomLeft
 *          = 3 = bottomRight
 **/
static void renderDotPatch(cairo_pattern_t* mesh,
                           int patchNum,
                           const Point &center,
                           double internalDotRadius,
                           double externalDotRadius,
                           double shapeColor[3],
                           double opacity)
{
    //inner circle
    Point p0,p1,p2,p3,p1p2,p2p1;
    
    //feather
    Point p0f,p0p1f,p1p0f,p1f,p1p2f,p2p1f,p2f,p2p3f,p3p2f,p3f,p3p0f,p0p3f;
    p0 = p3 = center; //< degenerated patch with only 3 sides
    
    //http://itc.ktu.lt/itc354/Riskus354.pdf
    //c = (4/3)*(sqrt(2)-1)
    const double c = 0.5522847498;
    
    if (patchNum == 0) {
        p1.x = center.x + internalDotRadius;
        p1.y = center.y;
        p2.x = center.x;
        p2.y = center.y + internalDotRadius;
        p1p2.x = p1.x;
        p1p2.y = center.y + c * internalDotRadius;
        p2p1.x = center.x + c * internalDotRadius;
        p2p1.y = p2.y;
        
        p1f.x = center.x + externalDotRadius;
        p1f.y = center.y;
        p2f.x = center.x;
        p2f.y = center.y + externalDotRadius;
        p1p2f.x = p1f.x;
        p1p2f.y = center.y + c * externalDotRadius;
        p2p1f.x = center.x + c * externalDotRadius;
        p2p1f.y = p2f.y;

        
    } else if (patchNum == 1) {
        p1.x = center.x;
        p1.y = center.y + internalDotRadius;
        p2.x = center.x - internalDotRadius;
        p2.y = center.y;
        p1p2.x = center.x - c * internalDotRadius;
        p1p2.y = p1.y;
        p2p1.x = p2.x;
        p2p1.y = center.y + c * internalDotRadius;
        
        p1f.x = center.x;
        p1f.y = center.y + externalDotRadius;
        p2f.x = center.x - externalDotRadius;
        p2f.y = center.y;
        p1p2f.x = center.x - c * externalDotRadius;
        p1p2f.y = p1f.y;
        p2p1f.x = p2f.x;
        p2p1f.y = center.y + c * externalDotRadius;

    } else if (patchNum == 2) {
        p1.x = center.x - internalDotRadius;
        p1.y = center.y;
        p2.x = center.x;
        p2.y = center.y - internalDotRadius;
        p1p2.x = p1.x;
        p1p2.y = center.y - c * internalDotRadius;
        p2p1.x = center.x - c * internalDotRadius;
        p2p1.y = p2.y;
        
        p1f.x = center.x - externalDotRadius;
        p1f.y = center.y;
        p2f.x = center.x;
        p2f.y = center.y - externalDotRadius;
        p1p2f.x = p1f.x;
        p1p2f.y = center.y - c * externalDotRadius;
        p2p1f.x = center.x - c * externalDotRadius;
        p2p1f.y = p2f.y;
    } else {
        assert(patchNum == 3);
        p1.x = center.x;
        p1.y = center.y - internalDotRadius;
        p2.x = center.x + internalDotRadius;
        p2.y = center.y;
        p1p2.x = center.x + c * internalDotRadius;
        p1p2.y = p1.y;
        p2p1.x = p2.x;
        p2p1.y = center.y - c * internalDotRadius;
        
        p1f.x = center.x;
        p1f.y = center.y - externalDotRadius;
        p2f.x = center.x + externalDotRadius;
        p2f.y = center.y;
        p1p2f.x = center.x + c * externalDotRadius;
        p1p2f.y = p1f.y;
        p2p1f.x = p2f.x;
        p2p1f.y = center.y - c * externalDotRadius;


    }
    
    p0f = p1;
    p3f = p2;
    p3p0f = p2p1;
    p0p3f = p1p2;
    
    ///linear interpolation
    
    p0p1f.x = (2. * p0f.x + p1f.x)  / 3.;
    p0p1f.y = (2. * p0f.y + p1f.y)  / 3.;
    p1p0f.x = (2. * p0f.x + p1f.x)  / 3.;
    p1p0f.y = (2. * p0f.y + p1f.y)  / 3.;
    
    
    p2p3f.x = (2. * p3f.x + p2f.x) / 3.;
    p2p3f.y = (2. * p3f.y + p2f.y) / 3.;
    p3p2f.x = (2. * p3f.x + p2f.x) / 3.;
    p3p2f.y = (2. * p3f.y + p2f.y) / 3.;

    
    
    ///move to the initial point
    cairo_mesh_pattern_begin_patch(mesh);
    cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
    cairo_mesh_pattern_line_to(mesh, p1.x, p1.y);
    cairo_mesh_pattern_curve_to(mesh, p1p2.x, p1p2.y, p2p1.x, p2p1.y, p2.x, p2.y);
    cairo_mesh_pattern_line_to(mesh, p3.x, p3.y);
    cairo_mesh_pattern_line_to(mesh, p0.x, p0.y);
    
    // IMPORTANT NOTE:
    // The two sqrt below are due to a probable cairo bug.
    // To check wether the bug is present is a given cairo version,
    // make any shape with a very large feather and set
    // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
    // and approximately equal to 0.5.
    // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
    // older Cairo versions.
    cairo_mesh_pattern_set_corner_color_rgba( mesh, 0, shapeColor[0], shapeColor[1], shapeColor[2],std::sqrt(opacity) );
    ///outter is faded
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor[0], shapeColor[1], shapeColor[2],opacity);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor[0], shapeColor[1], shapeColor[2],opacity);
    ///inner is full color
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor[0], shapeColor[1], shapeColor[2],std::sqrt(opacity));
    
    assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
    
    cairo_mesh_pattern_end_patch(mesh);
    
    
    ///move to the initial point
    cairo_mesh_pattern_begin_patch(mesh);
    cairo_mesh_pattern_move_to(mesh, p0f.x, p0f.y);
    cairo_mesh_pattern_curve_to(mesh, p0p1f.x, p0p1f.y, p1p0f.x, p1p0f.y, p1f.x, p1f.y);
    cairo_mesh_pattern_curve_to(mesh, p1p2f.x, p1p2f.y, p2p1f.x, p2p1f.y, p2f.x, p2f.y);
    cairo_mesh_pattern_curve_to(mesh, p2p3f.x, p2p3f.y, p3p2f.x, p3p2f.y, p3f.x, p3f.y);
    cairo_mesh_pattern_curve_to(mesh, p3p0f.x, p3p0f.y, p0p3f.x, p0p3f.y, p0f.x, p0f.y);
    
    // IMPORTANT NOTE:
    // The two sqrt below are due to a probable cairo bug.
    // To check wether the bug is present is a given cairo version,
    // make any shape with a very large feather and set
    // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
    // and approximately equal to 0.5.
    // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
    // older Cairo versions.
    cairo_mesh_pattern_set_corner_color_rgba( mesh, 0, shapeColor[0], shapeColor[1], shapeColor[2],std::sqrt(opacity) );
    ///outter is faded
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor[0], shapeColor[1], shapeColor[2],0);
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor[0], shapeColor[1], shapeColor[2],0);
    ///inner is full color
    cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor[0], shapeColor[1], shapeColor[2],std::sqrt(opacity));
    
    assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
    
    cairo_mesh_pattern_end_patch(mesh);

}


void
RotoContextPrivate::renderStroke(cairo_t* cr,const RotoStrokeItem* stroke, int time, unsigned int mipmapLevel)
{
    std::list<std::pair<Point,double> > points;

#ifdef ROTO_STROKE_USE_FIT_CURVE
    BezierCPs cps = stroke->getControlPoints_mt_safe();
    if (!stroke->isActivated(time) || cps.empty()) {
        return;
    }
    
    
    
    /**
     To determine the number of line segments the curve should have once subdivided, we approximate the length of the bezier.
     From http://antigrain.com/research/adaptive_bezier/ 
     
     Apparently, we need to calculate the step on the basis of the length of the curve.
     To do that we need to calculate the actual length of the curve, but to calculate the length we need to calculate the curve itself.
     It's a classical “catch 22” situation. A rather good estimation is the sum of the distances:
     (p1,p2)+(p2,p3)+(p3,p4);
     **/
    stroke->evaluateAtTime_DeCasteljau_autoNbPoints(time, mipmapLevel, &points, 0);
#else
    stroke->evaluateStroke(mipmapLevel, &points);
#endif
    
    if (points.empty()) {
        return;
    }
    
    double opacity = stroke->getOpacity(time);
    int operatorIndex = stroke->getCompositingOperator();
    double shapeColor[3];
    stroke->getColor(time, shapeColor);
    boost::shared_ptr<Double_Knob> brushSizeKnob = stroke->getBrushSizeKnob();
    double brushSize = brushSizeKnob->getValueAtTime(time);
    boost::shared_ptr<Double_Knob> brushSpacingKnob = stroke->getBrushSpacingKnob();
    double brushSpacing = brushSpacingKnob->getValueAtTime(time);
    if (brushSpacing == 0.) {
        return;
    }
    
    boost::shared_ptr<Double_Knob> brushHardnessKnob = stroke->getBrushHardnessKnob();
    double brushHardness = brushHardnessKnob->getValueAtTime(time);
    boost::shared_ptr<Double_Knob> brushEffectStrengthKnob = stroke->getBrushEffectKnob();
    double effectStrength = brushEffectStrengthKnob->getValueAtTime(time);
    boost::shared_ptr<Double_Knob> visiblePortionKnob = stroke->getBrushVisiblePortionKnob();
    double writeOnStart = visiblePortionKnob->getValueAtTime(time, 0);
    double writeOnEnd = visiblePortionKnob->getValueAtTime(time, 1);
    if ((writeOnEnd - writeOnStart) <= 0.) {
        return;
    }
    
    int firstPoint = (int)std::floor((points.size() * writeOnStart));
    int endPoint = (int)std::ceil((points.size() * writeOnEnd));
    assert(firstPoint >= 0 && firstPoint < (int)points.size() && endPoint > firstPoint && endPoint <= (int)points.size());
    
    
    
    cairo_set_operator(cr, (cairo_operator_t)operatorIndex);

    
    ///The visible portion of the paint's stroke with points adjusted to pixel coordinates
    std::list<std::pair<Point,double> > visiblePortion;
    std::list<std::pair<Point,double> >::iterator startingIt = points.begin();
    std::list<std::pair<Point,double> >::iterator endingIt = points.begin();
    std::advance(startingIt, firstPoint);
    std::advance(endingIt, endPoint);
    for (std::list<std::pair<Point,double> >::iterator it = startingIt; it!=endingIt; ++it) {
        visiblePortion.push_back(*it);
    }
    
    double brushSizePixel = brushSize;
    if (mipmapLevel != 0) {
        brushSizePixel /= (1 << mipmapLevel);
    }

    double internalDotRadius = std::max(brushSizePixel * brushHardness,1.) / 2.;
    double externalDotRadius = std::max(brushSizePixel, 1.) / 2.;
    double spacingPixel = externalDotRadius * 2. * brushSpacing;
    

    for (std::list<std::pair<Point,double> >::iterator it = visiblePortion.begin(); it!=visiblePortion.end();) {
        //Render for each point a dot. Spacing is a percentage of brushSize:
        //Spacing at 1 means no dot is overlapping another (so the spacing is in fact brushSize)
        //Spacing at 0 we do not render the stroke
        //A dot is a combination of 4 mesh patterns: an upper semi circle divided in 2 and lower semi circle divided in 2
        //the brush hardness is the strength of the feather relative to the radius of the dot: 1 means there is no feather
        //0 means the feather expands to the center of the dot
        
        ////Define the feather edge pattern
        cairo_pattern_t* mesh = cairo_pattern_create_mesh();
        if (cairo_pattern_status(mesh) != CAIRO_STATUS_SUCCESS) {
            cairo_pattern_destroy(mesh);
            return;
        }
        
        Point center;
        center.x = it->first.x;
        center.y = it->first.y;
        double pressure = it->second;
        for (int i = 0; i < 4; ++i) {
            renderDotPatch(mesh, i, center, internalDotRadius, externalDotRadius, shapeColor, opacity);
        }
        
        //Find the next point that we should draw a dot on according to the spacing in pixel coordinates
        std::list<std::pair<Point,double> >::iterator it2 = it;
        ++it2;
        double segmentDis = -1;
        for (; it2!=visiblePortion.end(); ++it2) {
            double dist = std::sqrt((it2->first.x - it->first.x) * (it2->first.x - it->first.x) + (it2->first.y - it->first.y) *
                                (it2->first.y - it->first.y));
            if (segmentDis == -1) {
                segmentDis = dist;
            }
            if (dist > (spacingPixel - segmentDis - 1)) {
                break;
            }
        }
        it = it2;
        
        applyAndDestroyMask(cr, mesh);

       
    }
    
}

void
RotoContextPrivate::renderBezier(cairo_t* cr,const Bezier* bezier,int time, unsigned int mipmapLevel)
{
    ///render the bezier only if finished (closed) and activated
    if ( !bezier->isCurveFinished() || !bezier->isActivated(time) || ( bezier->getControlPointsCount() <= 1 ) ) {
        return;
    }
    
    
    double fallOff = bezier->getFeatherFallOff(time);
    double featherDist = bezier->getFeatherDistance(time);
    double opacity = bezier->getOpacity(time);
#ifdef NATRON_ROTO_INVERTIBLE
    bool inverted = (*it2)->getInverted(time);
#else
    const bool inverted = false;
#endif
    int operatorIndex = bezier->getCompositingOperator();
    double shapeColor[3];
    bezier->getColor(time, shapeColor);
    
    cairo_set_operator(cr, (cairo_operator_t)operatorIndex);
    
    BezierCPs cps = bezier->getControlPoints_mt_safe();
    
    BezierCPs fps = bezier->getFeatherPoints_mt_safe();
    
    assert( cps.size() == fps.size() );
    
    if ( cps.empty() ) {
        return;
    }
    
    cairo_new_path(cr);
    
    ////Define the feather edge pattern
    cairo_pattern_t* mesh = cairo_pattern_create_mesh();
    if (cairo_pattern_status(mesh) != CAIRO_STATUS_SUCCESS) {
        cairo_pattern_destroy(mesh);
        return;
    }
    
    ///Adjust the feather distance so it takes the mipmap level into account
    if (mipmapLevel != 0) {
        featherDist /= (1 << mipmapLevel);
    }
    
    
    renderFeather(bezier, time, mipmapLevel, inverted, shapeColor, opacity, featherDist, fallOff, mesh);
    
    
    if (!inverted) {
        // strangely, the above-mentioned cairo bug doesn't affect this function
        renderInternalShape(time, mipmapLevel, shapeColor, opacity, cr, mesh, cps);
#ifdef NATRON_ROTO_INVERTIBLE
    } else {
#pragma message WARN("doesn't work! the image should be infinite for this to work!")
        // Doesn't work! the image should be infinite for this to work!
        // Or at least it should contain the Union of the source RoDs.
        // Here, it only contains the boinding box of the Bezier.
        // If there's a transform after the roto node, a black border will appear.
        // The only solution would be to have a color parameter which specifies how on image is outside of its RoD.
        // Unfortunately, the OFX definition is: "it is black and transparent"
        
        ///If inverted, draw an inverted rectangle on all the image first
        // with a hole consisting of the feather polygon
        
        double xOffset, yOffset;
        cairo_surface_get_device_offset(cairoImg, &xOffset, &yOffset);
        int width = cairo_image_surface_get_width(cairoImg);
        int height = cairo_image_surface_get_height(cairoImg);
        
        cairo_move_to(cr, -xOffset, -yOffset);
        cairo_line_to(cr, -xOffset + width, -yOffset);
        cairo_line_to(cr, -xOffset + width, -yOffset + height);
        cairo_line_to(cr, -xOffset, -yOffset + height);
        cairo_line_to(cr, -xOffset, -yOffset);
        // strangely, the above-mentioned cairo bug doesn't affect this function
#pragma message WARN("WRONG! should use the outer feather contour, *displaced* by featherDistance, not fps")
        renderInternalShape(time, mipmapLevel, cr, fps);
#endif
    }
    
    applyAndDestroyMask(cr, mesh);

}

void
RotoContextPrivate::renderFeather(const Bezier* bezier,int time, unsigned int mipmapLevel, bool inverted, double shapeColor[3], double opacity, double featherDist, double fallOff, cairo_pattern_t* mesh)
{
    
    double fallOffInverse = 1. / fallOff;
    /*
     * We descretize the feather control points to obtain a polygon so that the feather distance will be of the same thickness around all the shape.
     * If we were to extend only the end points, the resulting bezier interpolation would create a feather with different thickness around the shape,
     * yielding an unwanted behaviour for the end user.
     */
    ///here is the polygon of the feather bezier
    ///This is used only if the feather distance is different of 0 and the feather points equal
    ///the control points in order to still be able to apply the feather distance.
    std::list<Point> featherPolygon;
    std::list<Point> bezierPolygon;
    RectD featherPolyBBox( std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity() );
    
    bezier->evaluateFeatherPointsAtTime_DeCasteljau(time, mipmapLevel, 50, true, &featherPolygon, &featherPolyBBox);
    bezier->evaluateAtTime_DeCasteljau(time, mipmapLevel, 50, &bezierPolygon, NULL);
    
    bool clockWise = bezier->isFeatherPolygonClockwiseOriented(time);
    
    assert( !featherPolygon.empty() && !bezierPolygon.empty());


    std::list<Point> featherContour;

    // prepare iterators
    std::list<Point>::iterator next = featherPolygon.begin();
    ++next;  // can only be valid since we assert the list is not empty
    if (next == featherPolygon.end()) {
        next = featherPolygon.begin();
    }
    std::list<Point>::iterator prev = featherPolygon.end();
    --prev; // can only be valid since we assert the list is not empty
    std::list<Point>::iterator bezIT = bezierPolygon.begin();
    std::list<Point>::iterator prevBez = bezierPolygon.end();
    --prevBez; // can only be valid since we assert the list is not empty

    // prepare p1
    double absFeatherDist = std::abs(featherDist);
    Point p1 = *featherPolygon.begin();
    double norm = sqrt( (next->x - prev->x) * (next->x - prev->x) + (next->y - prev->y) * (next->y - prev->y) );
    assert(norm != 0);
    double dx = -( (next->y - prev->y) / norm );
    double dy = ( (next->x - prev->x) / norm );

    if (!clockWise) {
        p1.x -= dx * absFeatherDist;
        p1.y -= dy * absFeatherDist;
    } else {
        p1.x += dx * absFeatherDist;
        p1.y += dy * absFeatherDist;
    }
    
    Point origin = p1;
    featherContour.push_back(p1);


    // increment for first iteration
    std::list<Point>::iterator cur = featherPolygon.begin();
    // ++cur, ++prev, ++next, ++bezIT, ++prevBez
    // all should be valid, actually
    assert(cur != featherPolygon.end() &&
           prev != featherPolygon.end() &&
           next != featherPolygon.end() &&
           bezIT != bezierPolygon.end() &&
           prevBez != bezierPolygon.end());
    if (cur != featherPolygon.end()) {
        ++cur;
    }
    if (prev != featherPolygon.end()) {
        ++prev;
    }
    if (next != featherPolygon.end()) {
        ++next;
    }
    if (bezIT != bezierPolygon.end()) {
        ++bezIT;
    }
    if (prevBez != bezierPolygon.end()) {
        ++prevBez;
    }

    for (;; ++cur) { // for each point in polygon
        if ( next == featherPolygon.end() ) {
            next = featherPolygon.begin();
        }
        if ( prev == featherPolygon.end() ) {
            prev = featherPolygon.begin();
        }
        if ( bezIT == bezierPolygon.end() ) {
            bezIT = bezierPolygon.begin();
        }
        if ( prevBez == bezierPolygon.end() ) {
            prevBez = bezierPolygon.begin();
        }
        bool mustStop = false;
        if ( cur == featherPolygon.end() ) {
            mustStop = true;
            cur = featherPolygon.begin();
        }
        
        ///skip it
        if ( (cur->x == prev->x) && (cur->y == prev->y) ) {
            continue;
        }
        
        Point p0, p0p1, p1p0, p2, p2p3, p3p2, p3;
        p0.x = prevBez->x;
        p0.y = prevBez->y;
        p3.x = bezIT->x;
        p3.y = bezIT->y;
        
        if (!mustStop) {
            norm = sqrt( (next->x - prev->x) * (next->x - prev->x) + (next->y - prev->y) * (next->y - prev->y) );
            assert(norm != 0);
            dx = -( (next->y - prev->y) / norm );
            dy = ( (next->x - prev->x) / norm );
            p2 = *cur;

            if (!clockWise) {
                p2.x -= dx * absFeatherDist;
                p2.y -= dy * absFeatherDist;
            } else {
                p2.x += dx * absFeatherDist;
                p2.y += dy * absFeatherDist;
            }
        } else {
            p2 = origin;
        }
        featherContour.push_back(p2);
        
        ///linear interpolation
        p0p1.x = (p0.x * fallOff * 2. + fallOffInverse * p1.x) / (fallOff * 2. + fallOffInverse);
        p0p1.y = (p0.y * fallOff * 2. + fallOffInverse * p1.y) / (fallOff * 2. + fallOffInverse);
        p1p0.x = (p0.x * fallOff + 2. * fallOffInverse * p1.x) / (fallOff + 2. * fallOffInverse);
        p1p0.y = (p0.y * fallOff + 2. * fallOffInverse * p1.y) / (fallOff + 2. * fallOffInverse);

        
        p2p3.x = (p3.x * fallOff + 2. * fallOffInverse * p2.x) / (fallOff + 2. * fallOffInverse);
        p2p3.y = (p3.y * fallOff + 2. * fallOffInverse * p2.y) / (fallOff + 2. * fallOffInverse);
        p3p2.x = (p3.x * fallOff * 2. + fallOffInverse * p2.x) / (fallOff * 2. + fallOffInverse);
        p3p2.y = (p3.y * fallOff * 2. + fallOffInverse * p2.y) / (fallOff * 2. + fallOffInverse);
        
        
        ///move to the initial point
        cairo_mesh_pattern_begin_patch(mesh);
        cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
        cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
        cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
        cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
        cairo_mesh_pattern_line_to(mesh, p0.x, p0.y);
        ///Set the 4 corners color
        ///inner is full color
        
        // IMPORTANT NOTE:
        // The two sqrt below are due to a probable cairo bug.
        // To check wether the bug is present is a given cairo version,
        // make any shape with a very large feather and set
        // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
        // and approximately equal to 0.5.
        // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
        // older Cairo versions.
        cairo_mesh_pattern_set_corner_color_rgba( mesh, 0, shapeColor[0], shapeColor[1], shapeColor[2],
                                                 std::sqrt(inverted ? 1. - opacity : opacity) );
        ///outter is faded
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor[0], shapeColor[1], shapeColor[2],
                                                 inverted ? 1. : 0.);
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor[0], shapeColor[1], shapeColor[2],
                                                 inverted ? 1. : 0.);
        ///inner is full color
        cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor[0], shapeColor[1], shapeColor[2],
                                                 std::sqrt(inverted ? 1. - opacity : opacity));
        assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
        
        cairo_mesh_pattern_end_patch(mesh);
        
        if (mustStop) {
            break;
        }
        
        p1 = p2;

        // increment for next iteration
        // ++prev, ++next, ++bezIT, ++prevBez
        if (prev != featherPolygon.end()) {
            ++prev;
        }
        if (next != featherPolygon.end()) {
            ++next;
        }
        if (bezIT != bezierPolygon.end()) {
            ++bezIT;
        }
        if (prevBez != bezierPolygon.end()) {
            ++prevBez;
        }

    }  // for each point in polygon

}

void
RotoContextPrivate::renderInternalShape(int time,
                                        unsigned int mipmapLevel,
                                        double shapeColor[3],
                                        double opacity,
                                        cairo_t* cr,
                                        cairo_pattern_t* mesh,
                                        const BezierCPs & cps)
{
    assert(!cps.empty());
#ifdef ROTO_USE_MESH_PATTERN_ONLY
    std::list<BezierCPs> coonPatches;
    bezulate(time, cps, &coonPatches);
    
    for (std::list<BezierCPs>::iterator it = coonPatches.begin(); it != coonPatches.end(); ++it) {
        
        std::list<BezierCPs> fixedPatch;
        Natron::regularize(*it, time, &fixedPatch);
        for (std::list<BezierCPs>::iterator it2 = fixedPatch.begin(); it2 != fixedPatch.end(); ++it2) {
            
            
            
            std::size_t size = it2->size();
            assert(size <= 4 && size >= 2);
            
            BezierCPs::iterator patchIT = it2->begin();
            boost::shared_ptr<BezierCP> p0ptr,p1ptr,p2ptr,p3ptr;
            p0ptr = *patchIT;
            ++patchIT;
            if (size == 2) {
                p1ptr = p0ptr;
                p2ptr = *patchIT;
                p3ptr = p2ptr;
            } else if (size == 3) {
                p1ptr = *patchIT;
                p2ptr = *patchIT;
                ++patchIT;
                p3ptr = *patchIT;
            } else if (size == 4) {
                p1ptr = *patchIT;
                ++patchIT;
                p2ptr = *patchIT;
                ++patchIT;
                p3ptr = *patchIT;
            }
            assert(p0ptr && p1ptr && p2ptr && p3ptr);
            
            Point p0,p0p1,p1p0,p1,p1p2,p2p1,p2p3,p3p2,p2,p3,p3p0,p0p3;
            
            p0ptr->getLeftBezierPointAtTime(time, &p0p3.x, &p0p3.y);
            p0ptr->getPositionAtTime(time, &p0.x, &p0.y);
            p0ptr->getRightBezierPointAtTime(time, &p0p1.x, &p0p1.y);
            
            p1ptr->getLeftBezierPointAtTime(time, &p1p0.x, &p1p0.y);
            p1ptr->getPositionAtTime(time, &p1.x, &p1.y);
            p1ptr->getRightBezierPointAtTime(time, &p1p2.x, &p1p2.y);
            
            p2ptr->getLeftBezierPointAtTime(time, &p2p1.x, &p2p1.y);
            p2ptr->getPositionAtTime(time, &p2.x, &p2.y);
            p2ptr->getRightBezierPointAtTime(time, &p2p3.x, &p2p3.y);
            
            p3ptr->getLeftBezierPointAtTime(time, &p3p2.x, &p3p2.y);
            p3ptr->getPositionAtTime(time, &p3.x, &p3.y);
            p3ptr->getRightBezierPointAtTime(time, &p3p0.x, &p3p0.y);
            
            
            adjustToPointToScale(mipmapLevel, p0.x, p0.y);
            adjustToPointToScale(mipmapLevel, p0p1.x, p0p1.y);
            adjustToPointToScale(mipmapLevel, p1p0.x, p1p0.y);
            adjustToPointToScale(mipmapLevel, p1.x, p1.y);
            adjustToPointToScale(mipmapLevel, p1p2.x, p1p2.y);
            adjustToPointToScale(mipmapLevel, p2p1.x, p2p1.y);
            adjustToPointToScale(mipmapLevel, p2.x, p2.y);
            adjustToPointToScale(mipmapLevel, p2p3.x, p2p3.y);
            adjustToPointToScale(mipmapLevel, p3p2.x, p3p2.y);
            adjustToPointToScale(mipmapLevel, p3.x, p3.y);
            adjustToPointToScale(mipmapLevel, p3p0.x, p3p0.y);
            adjustToPointToScale(mipmapLevel, p0p3.x, p0p3.y);
            
            
            /*
             Add a Coons patch such as:
             
             C1  Side 1   C2
             +---------------+
             |               |
             |  P1       P2  |
             |               |
             Side 0 |               | Side 2
             |               |
             |               |
             |  P0       P3  |
             |               |
             +---------------+
             C0     Side 3   C3
             
             In the above drawing, C0 is p0, P0 is p0p1, P1 is p1p0, C1 is p1 and so on...
             */
            
            ///move to C0
            cairo_mesh_pattern_begin_patch(mesh);
            cairo_mesh_pattern_move_to(mesh, p0.x, p0.y);
            if (size == 4) {
                cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
                cairo_mesh_pattern_curve_to(mesh, p1p2.x, p1p2.y, p2p1.x, p2p1.y, p2.x, p2.y);
                cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            } else if (size == 3) {
                cairo_mesh_pattern_curve_to(mesh, p0p1.x, p0p1.y, p1p0.x, p1p0.y, p1.x, p1.y);
                cairo_mesh_pattern_line_to(mesh, p2.x, p2.y);
                cairo_mesh_pattern_curve_to(mesh, p2p3.x, p2p3.y, p3p2.x, p3p2.y, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            } else {
                assert(size == 2);
                cairo_mesh_pattern_line_to(mesh, p1.x, p1.y);
                cairo_mesh_pattern_curve_to(mesh, p1p2.x, p1p2.y, p2p1.x, p2p1.y, p2.x, p2.y);
                cairo_mesh_pattern_line_to(mesh, p3.x, p3.y);
                cairo_mesh_pattern_curve_to(mesh, p3p0.x, p3p0.y, p0p3.x, p0p3.y, p0.x, p0.y);
            }
            ///Set the 4 corners color
            
            // IMPORTANT NOTE:
            // The two sqrt below are due to a probable cairo bug.
            // To check wether the bug is present is a given cairo version,
            // make any shape with a very large feather and set
            // opacity to 0.5. Then, zoom on the polygon border to check if the intensity is continuous
            // and approximately equal to 0.5.
            // If the bug if ixed in cairo, please use #if CAIRO_VERSION>xxx to keep compatibility with
            // older Cairo versions.
            cairo_mesh_pattern_set_corner_color_rgba( mesh, 0, shapeColor[0], shapeColor[1], shapeColor[2],
                                                     std::sqrt(opacity) );
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 1, shapeColor[0], shapeColor[1], shapeColor[2],
                                                     opacity);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 2, shapeColor[0], shapeColor[1], shapeColor[2],
                                                     opacity);
            cairo_mesh_pattern_set_corner_color_rgba(mesh, 3, shapeColor[0], shapeColor[1], shapeColor[2],
                                                     std::sqrt(opacity));
            assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
            
            cairo_mesh_pattern_end_patch(mesh);
            
        }
    }
#else
    
    cairo_set_source_rgba(cr, shapeColor[0], shapeColor[1], shapeColor[2], opacity);
    
    BezierCPs::const_iterator point = cps.begin();
    assert(point != cps.end());
    BezierCPs::const_iterator nextPoint = point;
    if (nextPoint != cps.end()) {
        ++nextPoint;
    }

    Point initCp;
    (*point)->getPositionAtTime(time, &initCp.x,&initCp.y);
    adjustToPointToScale(mipmapLevel,initCp.x,initCp.y);

    cairo_move_to(cr, initCp.x,initCp.y);

    while ( point != cps.end() ) {
        if ( nextPoint == cps.end() ) {
            nextPoint = cps.begin();
        }

        double rightX,rightY,nextX,nextY,nextLeftX,nextLeftY;
        (*point)->getRightBezierPointAtTime(time, &rightX, &rightY);
        (*nextPoint)->getLeftBezierPointAtTime(time, &nextLeftX, &nextLeftY);
        (*nextPoint)->getPositionAtTime(time, &nextX, &nextY);

        adjustToPointToScale(mipmapLevel,rightX,rightY);
        adjustToPointToScale(mipmapLevel,nextX,nextY);
        adjustToPointToScale(mipmapLevel,nextLeftX,nextLeftY);
        cairo_curve_to(cr, rightX, rightY, nextLeftX, nextLeftY, nextX, nextY);

        // increment for next iteration
        ++point;
        if (nextPoint != cps.end()) {
            ++nextPoint;
        }
    } // while()
//    if (cairo_get_antialias(cr) != CAIRO_ANTIALIAS_NONE ) {
//        cairo_fill_preserve(cr);
//        // These line properties make for a nicer looking polygon mesh
//        cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);
//        cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
//        // Comment out the following call to cairo_set_line width
//        // since the hard-coded width value of 1.0 is not appropriate
//        // for fills of small areas. Instead, use the line width that
//        // has already been set by the user via the above call of
//        // poly_line which in turn calls set_current_context which in
//        // turn calls cairo_set_line_width for the user-specified
//        // width.
//        cairo_set_line_width(cr, 1.0);
//        cairo_stroke(cr);
//    } else {
    cairo_fill(cr);
//    }
#endif
}

struct qpointf_compare_less
{
    bool operator() (const QPointF& lhs,const QPointF& rhs) const
    {
        if (std::abs(lhs.x() - rhs.x()) < 1e-6) {
            if (std::abs(lhs.y() - rhs.y()) < 1e-6) {
                return false;
            } else if (lhs.y() < rhs.y()) {
                return true;
            } else {
                return false;
            }
        } else if (lhs.x() < rhs.x()) {
            return true;
        } else {
            return false;
        }
    }
};



//From http://www.math.ualberta.ca/~bowman/publications/cad10.pdf
void
RotoContextPrivate::bezulate(int time, const BezierCPs& cps,std::list<BezierCPs>* patches)
{
    BezierCPs simpleClosedCurve = cps;
    
    while (simpleClosedCurve.size() > 4) {
        
        bool found = false;
        for (int n = 3; n >= 2; --n) {
            
            assert((int)simpleClosedCurve.size() > n);
            
            //next points at point i + n
            BezierCPs::iterator next = simpleClosedCurve.begin();
            std::advance(next, n);
            
            std::list<Point> polygon;
            RectD bbox;
            bbox.x1 = std::numeric_limits<double>::infinity();
            bbox.x2 = -std::numeric_limits<double>::infinity();
            bbox.y1 = std::numeric_limits<double>::infinity();
            bbox.y2 = -std::numeric_limits<double>::infinity();
            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                Point p;
                (*it)->getPositionAtTime(time, &p.x, &p.y);
                polygon.push_back(p);
                if (p.x < bbox.x1) {
                    bbox.x1 = p.x;
                }
                if (p.x > bbox.x2) {
                    bbox.x2 = p.x;
                }
                if (p.y < bbox.y1) {
                    bbox.y1 = p.y;
                }
                if (p.y > bbox.y2) {
                    bbox.y2 = p.y;
                }
            }
            
            
            
            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                
                bool nextIsPassedEnd = false;
                if (next == simpleClosedCurve.end()) {
                    next = simpleClosedCurve.begin();
                    nextIsPassedEnd = true;
                }
                
                //mid-point of the line segment between points i and i + n
                Point nextPoint,curPoint;
                (*it)->getPositionAtTime(time, &curPoint.x, &curPoint.y);
                (*next)->getPositionAtTime(time, &nextPoint.x, &nextPoint.y);
                
                /*
                 * Compute the number of intersections between the current line segment [it,next] and all other line segments
                 * If the number of intersections is different of 2, ignore this segment.
                 */
                QLineF line(QPointF(curPoint.x,curPoint.y),QPointF(nextPoint.x,nextPoint.y));
                std::set<QPointF,qpointf_compare_less> intersections;
                std::list<Point>::const_iterator last_pt = polygon.begin();
                std::list<Point>::const_iterator cur = last_pt;
                ++cur;
                QPointF intersectionPoint;
                for (; cur != polygon.end(); ++cur, ++last_pt) {
                    QLineF polygonSegment(QPointF(last_pt->x,last_pt->y),QPointF(cur->x,cur->y));
                    if (line.intersect(polygonSegment, &intersectionPoint) == QLineF::BoundedIntersection) {
                        intersections.insert(intersectionPoint);
                    }
                    if (intersections.size() > 2) {
                        break;
                    }
                }
                
                if (intersections.size() != 2) {
                    continue;
                }
                
                /*
                 * Check if the midpoint of the line segment [it,next] lies inside the simple closed curve (polygon), otherwise
                 * ignore it.
                 */
                Point midPoint;
                midPoint.x = (nextPoint.x + curPoint.x) / 2.;
                midPoint.y = (nextPoint.y + curPoint.y) / 2.;
                bool isInside = pointInPolygon(midPoint,polygon,bbox,Bezier::eFillRuleWinding);

                if (isInside) {
                    
                    //Make the sub closed curve composed of the path from points i to i + n
                    BezierCPs subCurve;
                    subCurve.push_back(*it);
                    BezierCPs::iterator pointIt = it;
                    for (int i = 0; i < n - 1; ++i) {
                        ++pointIt;
                        if (pointIt == simpleClosedCurve.end()) {
                            pointIt = simpleClosedCurve.begin();
                        }
                        subCurve.push_back(*pointIt);
                    }
                    subCurve.push_back(*next);
                    
                    // Ensure that all interior angles are less than 180 degrees.
                    
                    
                    
                    patches->push_back(subCurve);
                    
                    //Remove i + 1 to i + n
                    BezierCPs::iterator eraseStart = it;
                    ++eraseStart;
                    bool eraseStartIsPassedEnd = false;
                    if (eraseStart == simpleClosedCurve.end()) {
                        eraseStart = simpleClosedCurve.begin();
                        eraseStartIsPassedEnd = true;
                    }
                    //"it" is  invalidated after the next instructions but we leave the loop anyway
                    assert(!simpleClosedCurve.empty());
                    if ((!nextIsPassedEnd && !eraseStartIsPassedEnd) || (nextIsPassedEnd && eraseStartIsPassedEnd)) {
                        simpleClosedCurve.erase(eraseStart,next);
                    } else {
                        simpleClosedCurve.erase(eraseStart,simpleClosedCurve.end());
                        if (!simpleClosedCurve.empty()) {
                            simpleClosedCurve.erase(simpleClosedCurve.begin(),next);
                        }
                    }
                    found = true;
                    break;
                }

                // increment for next iteration
                if (next != simpleClosedCurve.end()) {
                    ++next;
                }
            } // for(it)
            if (found) {
                break;
            }
        } // for(n)
        
        if (!found) {
            BezierCPs subdivisedCurve;
            //Subdivise the curve at the midpoint of each segment
            BezierCPs::iterator next = simpleClosedCurve.begin();
            if (next != simpleClosedCurve.end()) {
                ++next;
            }
            for (BezierCPs::iterator it = simpleClosedCurve.begin(); it != simpleClosedCurve.end(); ++it) {
                
                if (next == simpleClosedCurve.end()) {
                    next = simpleClosedCurve.begin();
                }
                Point p0,p1,p2,p3,p0p1, p1p2, p2p3, p0p1_p1p2, p1p2_p2p3,dest;
                (*it)->getPositionAtTime(time, &p0.x, &p0.y);
                (*it)->getRightBezierPointAtTime(time, &p1.x, &p1.y);
                (*next)->getLeftBezierPointAtTime(time, &p2.x, &p2.y);
                (*next)->getPositionAtTime(time, &p3.x, &p3.y);
                bezierFullPoint(p0, p1, p2, p3, 0.5, &p0p1, &p1p2, &p2p3, &p0p1_p1p2, &p1p2_p2p3, &dest);
                boost::shared_ptr<BezierCP> controlPoint(new BezierCP);
                controlPoint->setStaticPosition(dest.x, dest.y);
                controlPoint->setLeftBezierStaticPosition(p0p1_p1p2.x, p0p1_p1p2.y);
                controlPoint->setRightBezierStaticPosition(p1p2_p2p3.x, p1p2_p2p3.y);
                subdivisedCurve.push_back(*it);
                subdivisedCurve.push_back(controlPoint);

                // increment for next iteration
                if (next != simpleClosedCurve.end()) {
                    ++next;
                }
            } // for()
            simpleClosedCurve = subdivisedCurve;
        }
    }
    if (!simpleClosedCurve.empty()) {
        assert(simpleClosedCurve.size() >= 2);
        patches->push_back(simpleClosedCurve);
    }
}

void
RotoContextPrivate::applyAndDestroyMask(cairo_t* cr,
                                        cairo_pattern_t* mesh)
{
    assert(cairo_pattern_status(mesh) == CAIRO_STATUS_SUCCESS);
    cairo_set_source(cr, mesh);

    ///paint with the feather with the pattern as a mask
    cairo_mask(cr, mesh);

    cairo_pattern_destroy(mesh);
}

void
RotoContext::changeItemScriptName(const std::string& oldFullyQualifiedName,const std::string& newFullyQUalifiedName)
{
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = appID + "." + getNode()->getFullyQualifiedName();
    std::string err;
    
    std::string declStr = nodeName + ".roto." + newFullyQUalifiedName + " = " + nodeName + ".roto." + oldFullyQualifiedName + "\n";
    std::string delStr = "del " + nodeName + ".roto." + oldFullyQualifiedName + "\n";
    std::string script = declStr + delStr;
    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if (!Natron::interpretPythonScript(script , &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }
}

void
RotoContext::removeItemAsPythonField(const boost::shared_ptr<RotoItem>& item)
{
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = appID + "." + getNode()->getFullyQualifiedName();
    std::string err;
    std::string script = "del " + nodeName + ".roto." + item->getFullyQualifiedName() + "\n";
    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if (!Natron::interpretPythonScript(script , &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }
    
}


void
RotoContext::declareItemAsPythonField(const boost::shared_ptr<RotoItem>& item)
{
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = appID + "." + getNode()->getFullyQualifiedName();
    RotoLayer* isLayer = dynamic_cast<RotoLayer*>(item.get());
    
    std::string err;
    std::string script = nodeName + ".roto." + item->getFullyQualifiedName() + " = " +
    nodeName + ".roto.getItemByName(\"" + item->getScriptName() + "\")\n";
    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if(!Natron::interpretPythonScript(script , &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }
    
    if (isLayer) {
        const RotoItems& items = isLayer->getItems();
        for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
            declareItemAsPythonField(*it);
        }
    }
}

void
RotoContext::declarePythonFields()
{
    for (std::list< boost::shared_ptr<RotoLayer> >::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        declareItemAsPythonField(*it);
    }
}