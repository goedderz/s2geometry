// Copyright 2005 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS-IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// Author: ericv@google.com (Eric Veach)

#include "s2loop.h"

#include <float.h>
#include <algorithm>
#include <bitset>
#include <set>
#include <utility>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "base/atomicops.h"
#include "base/integral_types.h"
#include "base/stringprintf.h"
#include "util/coding/coder.h"
#include "r1interval.h"
#include "s1angle.h"
#include "s1interval.h"
#include "s2.h"
#include "s2cap.h"
#include "s2cell.h"
#include "s2closestedgequery.h"
#include "s2edgequery.h"
#include "s2edgeutil.h"
#include "s2error.h"
#include "s2paddedcell.h"
#include "s2pointcompression.h"
#include "s2shapeindex.h"
#include "s2shapeutil.h"
#include "util/math/matrix3x3.h"

using std::max;
using std::min;
using std::pair;
using std::set;
using std::vector;

DEFINE_bool(
    s2loop_lazy_indexing, true,
    "Build the S2ShapeIndex only when it is first needed.  This can save "
    "significant amounts of memory and time when geometry is constructed but "
    "never queried, for example when loops are passed directly to S2Polygon, "
    "or when geometry is being converted from one format to another.");

// The maximum number of vertices we'll allow when decoding a loop.
// The default value of 50 million is about 30x bigger than the number of
DEFINE_int32(
    s2polygon_decode_max_num_vertices, 50000000,
    "The upper limit on the number of loops that are allowed by the "
    "S2Polygon::Decode method.");

static const unsigned char kCurrentLosslessEncodingVersionNumber = 1;

// Boolean properties for compressed loops.
// See GetCompressedEncodingProperties.
enum CompressedLoopProperty {
  kOriginInside,
  kBoundEncoded,
  kNumProperties
};

S2Loop::S2Loop()
  : depth_(0),
    num_vertices_(0),
    vertices_(NULL),
    owns_vertices_(false),
    s2debug_override_(ALLOW_S2DEBUG),
    origin_inside_(false),
    shape_(this) {
  // Some fields are initialized by Init().  The loop is not valid until then.
}

S2Loop::S2Loop(vector<S2Point> const& vertices)
  : depth_(0),
    num_vertices_(0),
    vertices_(NULL),
    owns_vertices_(false),
    s2debug_override_(ALLOW_S2DEBUG),
    shape_(this) {
  Init(vertices);
}

S2Loop::S2Loop(vector<S2Point> const& vertices, S2debugOverride override)
  : depth_(0),
    num_vertices_(0),
    vertices_(NULL),
    owns_vertices_(false),
    s2debug_override_(override),
    shape_(this) {
  Init(vertices);
}

void S2Loop::set_s2debug_override(S2debugOverride override) {
  s2debug_override_ = override;
}

S2debugOverride S2Loop::s2debug_override() const {
  return static_cast<S2debugOverride>(s2debug_override_);
}

void S2Loop::ResetMutableFields() {
  base::subtle::NoBarrier_Store(&unindexed_contains_calls_, 0);
  index_.Reset();
}

void S2Loop::Init(vector<S2Point> const& vertices) {
  ResetMutableFields();
  if (owns_vertices_) delete[] vertices_;
  num_vertices_ = vertices.size();
  vertices_ = new S2Point[num_vertices_];
  std::copy(vertices.begin(), vertices.end(), &vertices_[0]);
  owns_vertices_ = true;
  InitOriginAndBound();
}

bool S2Loop::IsValid() const {
  S2Error error;
  if (FindValidationError(&error)) {
    LOG_IF(ERROR, FLAGS_s2debug) << error;
    return false;
  }
  return true;
}

bool S2Loop::FindValidationError(S2Error* error) const {
  return (FindValidationErrorNoIndex(error) ||
          s2shapeutil::FindSelfIntersection(index_, *this, error));
}

bool S2Loop::FindValidationErrorNoIndex(S2Error* error) const {
  // subregion_bound_ must be at least as large as bound_.  (This is an
  // internal consistency check rather than a test of client data.)
  DCHECK(subregion_bound_.Contains(bound_));

  // All vertices must be unit length.  (Unfortunately this check happens too
  // late in debug mode, because S2Loop construction calls S2::RobustCCW which
  // expects vertices to be unit length.  But it is still a useful check in
  // optimized builds.)
  for (int i = 0; i < num_vertices(); ++i) {
    if (!S2::IsUnitLength(vertex(i))) {
      error->Init(S2Error::NOT_UNIT_LENGTH,
                  "Vertex %d is not unit length", i);
      return true;
    }
  }
  // Loops must have at least 3 vertices (except for "empty" and "full").
  if (num_vertices() < 3) {
    if (is_empty_or_full()) {
      return false;  // Skip remaining tests.
    }
    error->Init(S2Error::LOOP_NOT_ENOUGH_VERTICES,
                "Non-empty, non-full loops must have at least 3 vertices");
    return true;
  }
  // Loops are not allowed to have any duplicate vertices or edge crossings.
  // We split this check into two parts.  First we check that no edge is
  // degenerate (identical endpoints).  Then we check that there are no
  // intersections between non-adjacent edges (including at vertices).  The
  // second part needs the S2ShapeIndex, so it does not fall within the scope
  // of this method.
  for (int i = 0; i < num_vertices(); ++i) {
    if (vertex(i) == vertex(i+1)) {
      error->Init(S2Error::DUPLICATE_VERTICES,
                  "Edge %d is degenerate (duplicate vertex)", i);
      return true;
    }
  }
  return false;
}

void S2Loop::InitOriginAndBound() {
  if (num_vertices() < 3) {
    // Check for the special "empty" and "full" loops (which have one vertex).
    if (!is_empty_or_full()) {
      origin_inside_ = false;
      return;  // Bail out without trying to access non-existent vertices.
    }
    // If the vertex is in the southern hemisphere then the loop is full,
    // otherwise it is empty.
    origin_inside_ = (vertex(0).z() < 0);
  } else {
    // Point containment testing is done by counting edge crossings starting
    // at a fixed point on the sphere (S2::Origin()).  Historically this was
    // important, but it is now no longer necessary, and it may be worthwhile
    // experimenting with using a loop vertex as the reference point.  In any
    // case, we need to know whether the reference point (S2::Origin) is
    // inside or outside the loop before we can construct the S2ShapeIndex.
    // We do this by first guessing that it is outside, and then seeing
    // whether we get the correct containment result for vertex 1.  If the
    // result is incorrect, the origin must be inside the loop.
    //
    // A loop with consecutive vertices A,B,C contains vertex B if and only if
    // the fixed vector R = S2::Ortho(B) is contained by the wedge ABC.  The
    // wedge is closed at A and open at C, i.e. the point B is inside the loop
    // if A=R but not if C=R.  This convention is required for compatibility
    // with S2EdgeUtil::VertexCrossing.  (Note that we can't use S2::Origin()
    // as the fixed vector because of the possibility that B == S2::Origin().)
    //
    // TODO(ericv): Investigate using vertex(0) as the reference point.

    origin_inside_ = false;  // Initialize before calling Contains().
    bool v1_inside = S2::OrderedCCW(S2::Ortho(vertex(1)), vertex(0),
                                    vertex(2), vertex(1));
    // Note that Contains(S2Point) only does a bounds check once InitIndex()
    // has been called, so it doesn't matter that bound_ is undefined here.
    if (v1_inside != Contains(vertex(1))) {
      origin_inside_ = true;
    }
  }
  // We *must* call InitBound() before InitIndex(), because InitBound() calls
  // Contains(S2Point), and Contains(S2Point) does a bounds check whenever the
  // index is not fresh (i.e., the loop has been added to the index but the
  // index has not been updated yet).
  //
  // TODO(ericv): When fewer S2Loop methods depend on internal bounds checks,
  // consider computing the bound on demand as well.
  InitBound();
  InitIndex();
}

void S2Loop::InitBound() {
  // Check for the special "empty" and "full" loops.
  if (is_empty_or_full()) {
    if (is_empty()) {
      subregion_bound_ = bound_ = S2LatLngRect::Empty();
    } else {
      subregion_bound_ = bound_ = S2LatLngRect::Full();
    }
    return;
  }

  // The bounding rectangle of a loop is not necessarily the same as the
  // bounding rectangle of its vertices.  First, the maximal latitude may be
  // attained along the interior of an edge.  Second, the loop may wrap
  // entirely around the sphere (e.g. a loop that defines two revolutions of a
  // candy-cane stripe).  Third, the loop may include one or both poles.
  // Note that a small clockwise loop near the equator contains both poles.

  S2EdgeUtil::RectBounder bounder;
  for (int i = 0; i <= num_vertices(); ++i) {
    bounder.AddPoint(&vertex(i));
  }
  S2LatLngRect b = bounder.GetBound();
  if (Contains(S2Point(0, 0, 1))) {
    b = S2LatLngRect(R1Interval(b.lat().lo(), M_PI_2), S1Interval::Full());
  }
  // If a loop contains the south pole, then either it wraps entirely
  // around the sphere (full longitude range), or it also contains the
  // north pole in which case b.lng().is_full() due to the test above.
  // Either way, we only need to do the south pole containment test if
  // b.lng().is_full().
  if (b.lng().is_full() && Contains(S2Point(0, 0, -1))) {
    b.mutable_lat()->set_lo(-M_PI_2);
  }
  bound_ = b;
  subregion_bound_ = S2EdgeUtil::RectBounder::ExpandForSubregions(bound_);
}

void S2Loop::InitIndex() {
  index_.Add(&shape_);
  if (!FLAGS_s2loop_lazy_indexing) {
    index_.ForceApplyUpdates();  // Force index construction now.
  }
  if (FLAGS_s2debug && s2debug_override_ == ALLOW_S2DEBUG) {
    // Note that FLAGS_s2debug is false in optimized builds (by default).
    CHECK(IsValid());
  }
}

S2Loop::S2Loop(S2Cell const& cell)
    : depth_(0),
      num_vertices_(4),
      vertices_(new S2Point[num_vertices_]),
      owns_vertices_(true),
      s2debug_override_(ALLOW_S2DEBUG),
      unindexed_contains_calls_(0),
      shape_(this) {
  for (int i = 0; i < 4; ++i) {
    vertices_[i] = cell.GetVertex(i);
  }
  // We recompute the bounding rectangle ourselves, since S2Cell uses a
  // different method and we need all the bounds to be consistent.
  InitOriginAndBound();
}

S2Loop::~S2Loop() {
  if (owns_vertices_) delete[] vertices_;
}

S2Loop::S2Loop(S2Loop const* src)
    : depth_(src->depth_),
      num_vertices_(src->num_vertices_),
      vertices_(new S2Point[num_vertices_]),
      owns_vertices_(true),
      s2debug_override_(src->s2debug_override_),
      origin_inside_(src->origin_inside_),
      unindexed_contains_calls_(0),
      bound_(src->bound_),
      subregion_bound_(src->subregion_bound_),
      shape_(this) {
  std::copy(&src->vertices_[0], &src->vertices_[num_vertices_], &vertices_[0]);
  InitIndex();
}

S2Loop* S2Loop::Clone() const {
  return new S2Loop(this);
}

int S2Loop::FindVertex(S2Point const& p) const {
  if (num_vertices() < 10) {
    // Exhaustive search.  Return value must be in the range [1..N].
    for (int i = 1; i <= num_vertices(); ++i) {
      if (vertex(i) == p) return i;
    }
    return -1;
  }
  S2ShapeIndex::Iterator it(index_);
  if (!it.Locate(p)) return -1;

  S2ClippedShape const& a_clipped = it.cell()->clipped(0);
  for (int i = a_clipped.num_edges() - 1; i >= 0; --i) {
    int ai = a_clipped.edge(i);
    // Return value must be in the range [1..N].
    if (vertex(ai) == p) return (ai == 0) ? num_vertices() : ai;
    if (vertex(ai+1) == p) return ai+1;
  }
  return -1;
}

bool S2Loop::IsNormalized() const {
  // Optimization: if the longitude span is less than 180 degrees, then the
  // loop covers less than half the sphere and is therefore normalized.
  if (bound_.lng().GetLength() < M_PI) return true;

  // We allow some error so that hemispheres are always considered normalized.
  // TODO(ericv): This is no longer required by the S2Polygon implementation,
  // so alternatively we could create the invariant that a loop is normalized
  // if and only if its complement is not normalized.
  return GetTurningAngle() >= -GetTurningAngleMaxError();
}

void S2Loop::Normalize() {
  CHECK(owns_vertices_);
  if (!IsNormalized()) Invert();
  DCHECK(IsNormalized());
}

void S2Loop::Invert() {
  CHECK(owns_vertices_);
  ResetMutableFields();
  if (is_empty_or_full()) {
    vertices_[0] = is_full() ? kEmptyVertex() : kFullVertex();
  } else {
    std::reverse(vertices_, vertices_ + num_vertices());
  }
  // origin_inside_ must be set correctly before building the S2ShapeIndex.
  origin_inside_ ^= true;
  if (bound_.lat().lo() > -M_PI_2 && bound_.lat().hi() < M_PI_2) {
    // The complement of this loop contains both poles.
    subregion_bound_ = bound_ = S2LatLngRect::Full();
  } else {
    InitBound();
  }
  InitIndex();
}

double S2Loop::GetArea() const {
  // It is suprisingly difficult to compute the area of a loop robustly.  The
  // main issues are (1) whether degenerate loops are considered to be CCW or
  // not (i.e., whether their area is close to 0 or 4*Pi), and (2) computing
  // the areas of small loops with good relative accuracy.
  //
  // With respect to degeneracies, we would like GetArea() to be consistent
  // with S2Loop::Contains(S2Point) in that loops that contain many points
  // should have large areas, and loops that contain few points should have
  // small areas.  For example, if a degenerate triangle is considered CCW
  // according to S2::RobustCCW(), then it will contain very few points and
  // its area should be approximately zero.  On the other hand if it is
  // considered clockwise, then it will contain virtually all points and so
  // its area should be approximately 4*Pi.

  // More precisely, let U be the set of S2Points for which S2::IsUnitLength()
  // is true, let P(U) be the projection of those points onto the mathematical
  // unit sphere, and let V(P(U)) be the Voronoi diagram of the projected
  // points.  Then for every loop x, we would like GetArea() to approximately
  // equal the sum of the areas of the Voronoi regions of the points p for
  // which x.Contains(p) is true.
  //
  // The second issue is that we want to compute the area of small loops
  // accurately.  This requires having good relative precision rather than
  // good absolute precision.  For example, if the area of a loop is 1e-12 and
  // the error is 1e-15, then the area only has 3 digits of accuracy.  (For
  // reference, 1e-12 is about 40 square meters on the surface of the earth.)
  // We would like to have good relative accuracy even for small loops.
  //
  // To achieve these goals, we combine two different methods of computing the
  // area.  This first method is based on the Gauss-Bonnet theorem, which says
  // that the area enclosed by the loop equals 2*Pi minus the total geodesic
  // curvature of the loop (i.e., the sum of the "turning angles" at all the
  // loop vertices).  The big advantage of this method is that as long as we
  // use S2::RobustCCW() to compute the turning angle at each vertex, then
  // degeneracies are always handled correctly.  In other words, if a
  // degenerate loop is CCW according to the symbolic perturbations used by
  // S2::RobustCCW(), then its turning angle will be approximately 2*Pi.
  //
  // The disadvantage of the Gauss-Bonnet method is that its absolute error is
  // about 2e-15 times the number of vertices (see GetTurningAngleMaxError).
  // So, it cannot compute the area of small loops accurately.
  //
  // The second method is based on splitting the loop into triangles and
  // summing the area of each triangle.  To avoid the difficulty and expense
  // of decomposing the loop into a union of non-overlapping triangles,
  // instead we compute a signed sum over triangles that may overlap (see the
  // comments for S2Loop::GetSurfaceIntegral).  The advantage of this method
  // is that the area of each triangle can be computed with much better
  // relative accuracy (using l'Huilier's theorem).  The disadvantage is that
  // the result is a signed area: CCW loops may yield a small positive value,
  // while CW loops may yield a small negative value (which is converted to a
  // positive area by adding 4*Pi).  This means that small errors in computing
  // the signed area may translate into a very large error in the result (if
  // the sign of the sum is incorrect).
  //
  // So, our strategy is to combine these two methods as follows.  First we
  // compute the area using the "signed sum over triangles" approach (since it
  // is generally more accurate).  We also estimate the maximum error in this
  // result.  If the signed area is too close to zero (i.e., zero is within
  // the error bounds), then we double-check the sign of the result using the
  // Gauss-Bonnet method.  (In fact we just call IsNormalized(), which is
  // based on this method.)  If the two methods disagree, we return either 0
  // or 4*Pi based on the result of IsNormalized().  Otherwise we return the
  // area that we computed originally.

  if (is_empty_or_full()) {
    return contains_origin() ? (4 * M_PI) : 0;
  }
  double area = GetSurfaceIntegral(S2::SignedArea);

  // TODO(ericv): This error estimate is very approximate.  There are two
  // issues: (1) SignedArea needs some improvements to ensure that its error
  // is actually never higher than GirardArea, and (2) although the number of
  // triangles in the sum is typically N-2, in theory it could be as high as
  // 2*N for pathological inputs.  But in other respects this error bound is
  // very conservative since it assumes that the maximum error is achieved on
  // every triangle.
  double max_error = GetTurningAngleMaxError();

  // The signed area should be between approximately -4*Pi and 4*Pi.
  DCHECK_LE(fabs(area), 4 * M_PI + max_error);
  if (area < 0) {
    // We have computed the negative of the area of the loop exterior.
    area += 4 * M_PI;
  }
  area = max(0.0, min(4 * M_PI, area));

  // If the area is close enough to zero or 4*Pi so that the loop orientation
  // is ambiguous, then we compute the loop orientation explicitly.
  if (area < max_error && !IsNormalized()) {
    return 4 * M_PI;
  } else if (area > (4 * M_PI - max_error) && IsNormalized()) {
    return 0.0;
  } else {
    return area;
  }
}

S2Point S2Loop::GetCentroid() const {
  // GetSurfaceIntegral() returns either the integral of position over loop
  // interior, or the negative of the integral of position over the loop
  // exterior.  But these two values are the same (!), because the integral of
  // position over the entire sphere is (0, 0, 0).
  return GetSurfaceIntegral(S2::TrueCentroid);
}

// Return (first, dir) such that first..first+n*dir are valid indices.
int S2Loop::GetCanonicalFirstVertex(int* dir) const {
  int first = 0;
  int n = num_vertices();
  for (int i = 1; i < n; ++i) {
    if (vertex(i) < vertex(first)) first = i;
  }
  if (vertex(first + 1) < vertex(first + n - 1)) {
    *dir = 1;
    // 0 <= first <= n-1, so (first+n*dir) <= 2*n-1.
  } else {
    *dir = -1;
    first += n;
    // n <= first <= 2*n-1, so (first+n*dir) >= 0.
  }
  return first;
}

S1Angle S2Loop::GetDistance(S2Point const& x) const {
  if (Contains(x)) return S1Angle::Zero();
  S2ClosestEdgeQuery query(index_);
  return query.GetDistance(x);
}

S1Angle S2Loop::GetDistanceToBoundary(S2Point const& x) const {
  S2ClosestEdgeQuery query(index_);
  return query.GetDistance(x);
}

S2Point S2Loop::Project(S2Point const& x) const {
  if (Contains(x)) return x;
  S2ClosestEdgeQuery query(index_);
  return query.Project(x);
}

S2Point S2Loop::ProjectToBoundary(S2Point const& x) const {
  S2ClosestEdgeQuery query(index_);
  return query.Project(x);
}

double S2Loop::GetTurningAngle() const {
  // For empty and full loops, we return the limit value as the loop area
  // approaches 0 or 4*Pi respectively.
  if (is_empty_or_full()) {
    return contains_origin() ? (-2 * M_PI) : (2 * M_PI);
  }
  // Don't crash even if the loop is not well-defined.
  if (num_vertices() < 3) return 0;

  // To ensure that we get the same result when the vertex order is rotated,
  // and that the result is negated when the vertex order is reversed, we need
  // to add up the individual turn angles in a consistent order.  (In general,
  // adding up a set of numbers in a different order can change the sum due to
  // rounding errors.)
  //
  // Furthermore, if we just accumulate an ordinary sum then the worst-case
  // error is quadratic in the number of vertices.  (This can happen with
  // spiral shapes, where the partial sum of the turning angles can be linear
  // in the number of vertices.)  To avoid this we use the Kahan summation
  // algorithm (http://en.wikipedia.org/wiki/Kahan_summation_algorithm).

  int n = num_vertices();
  int dir, i = GetCanonicalFirstVertex(&dir);
  double sum = S2::TurnAngle(vertex((i + n - dir) % n), vertex(i),
                             vertex((i + dir) % n));
  double compensation = 0;  // Kahan summation algorithm
  while (--n > 0) {
    i += dir;
    double angle = S2::TurnAngle(vertex(i - dir), vertex(i), vertex(i + dir));
    double old_sum = sum;
    angle += compensation;
    sum += angle;
    compensation = (old_sum - sum) + angle;
  }
  return dir * (sum + compensation);
}

double S2Loop::GetTurningAngleMaxError() const {
  // The maximum error can be bounded as follows:
  //   2.24 * DBL_EPSILON    for RobustCrossProd(b, a)
  //   2.24 * DBL_EPSILON    for RobustCrossProd(c, b)
  //   3.25 * DBL_EPSILON    for Angle()
  //   2.00 * DBL_EPSILON    for each addition in the Kahan summation
  //   ------------------
  //   9.73 * DBL_EPSILON
  double const kMaxErrorPerVertex = 9.73 * DBL_EPSILON;
  return kMaxErrorPerVertex * num_vertices();
}

S2Cap S2Loop::GetCapBound() const {
  return bound_.GetCapBound();
}

bool S2Loop::Contains(S2Cell const& target) const {
  S2ShapeIndex::Iterator it(index_);
  S2ShapeIndex::CellRelation relation = it.Locate(target.id());

  // If "target" is disjoint from all index cells, it is not contained.
  // Similarly, if "target" is subdivided into one or more index cells then it
  // is not contained, since index cells are subdivided only if they (nearly)
  // intersect a sufficient number of edges.  (But note that if "target" itself
  // is an index cell then it may be contained, since it could be a cell with
  // no edges in the loop interior.)
  if (relation != S2ShapeIndex::INDEXED) return false;

  // Otherwise check if any edges intersect "target".
  if (BoundaryApproxIntersects(it, target)) return false;

  // Otherwise check if the loop contains the center of "target".
  return Contains(it, target.GetCenter());
}

bool S2Loop::MayIntersect(S2Cell const& target) const {
  S2ShapeIndex::Iterator it(index_);
  S2ShapeIndex::CellRelation relation = it.Locate(target.id());

  // If "target" does not overlap any index cell, there is no intersection.
  if (relation == S2ShapeIndex::DISJOINT) return false;

  // If "target" is subdivided into one or more index cells, there is an
  // intersection to within the S2ShapeIndex error bound (see Contains).
  if (relation == S2ShapeIndex::SUBDIVIDED) return true;

  // If "target" is an index cell, there is an intersection because index cells
  // are created only if they have at least one edge or they are entirely
  // contained by the loop.
  if (it.id() == target.id()) return true;

  // Otherwise check if any edges intersect "target".
  if (BoundaryApproxIntersects(it, target)) return true;

  // Otherwise check if the loop contains the center of "target".
  return Contains(it, target.GetCenter());
}

bool S2Loop::BoundaryApproxIntersects(S2ShapeIndex::Iterator const& it,
                                      S2Cell const& target) const {
  DCHECK(it.id().contains(target.id()));
  S2ClippedShape const& a_clipped = it.cell()->clipped(0);
  int a_num_clipped = a_clipped.num_edges();

  // If there are no edges, there is no intersection.
  if (a_num_clipped == 0) return false;

  // We can save some work if "target" is the index cell itself.
  if (it.id() == target.id()) return true;

  // Otherwise check whether any of the edges intersect "target".
  static double const kMaxError = (S2EdgeUtil::kFaceClipErrorUVCoord +
                                   S2EdgeUtil::kIntersectsRectErrorUVDist);
  R2Rect bound = target.GetBoundUV().Expanded(kMaxError);
  for (int i = 0; i < a_num_clipped; ++i) {
    int ai = a_clipped.edge(i);
    R2Point v0, v1;
    if (S2EdgeUtil::ClipToPaddedFace(vertex(ai), vertex(ai+1), target.face(),
                                     kMaxError, &v0, &v1) &&
        S2EdgeUtil::IntersectsRect(v0, v1, bound)) {
      return true;
    }
  }
  return false;
}

bool S2Loop::Contains(S2Point const& p) const {
  // NOTE(ericv): A bounds check slows down this function by about 50%.  It is
  // worthwhile only when it might allow us to delay building the index.
  if (!index_.is_fresh() && !bound_.Contains(p)) return false;

  // For small loops it is faster to just check all the crossings.  We also
  // use this method during loop initialization because InitOriginAndBound()
  // calls Contains() before InitIndex().  Otherwise, we keep track of the
  // number of calls to Contains() and only build the index when enough calls
  // have been made so that we think it is worth the effort.  Note that the
  // code below is structured so that if many calls are made in parallel only
  // one thread builds the index, while the rest continue using brute force
  // until the index is actually available.
  //
  // The constants below were tuned using the benchmarks.  It turns out that
  // building the index costs roughly 50x as much as Contains().  (The ratio
  // increases slowly from 46x with 64 points to 61x with 256k points.)  The
  // textbook approach to this problem would be to wait until the cumulative
  // time we would have saved with an index approximately equals the cost of
  // building the index, and then build it.  (This gives the optimal
  // competitive ratio of 2; look up "competitive algorithms" for details.)
  // We set the limit somewhat lower than this (20 rather than 50) because
  // building the index may be forced anyway by other API calls, and so we
  // want to err on the side of building it too early.

  static int const kMaxBruteForceVertices = 32;
  static int const kMaxUnindexedContainsCalls = 20;  // See notes above.
  if (index_.num_shape_ids() == 0 ||  // InitIndex() not called yet
      num_vertices() <= kMaxBruteForceVertices ||
      (!index_.is_fresh() &&
       base::subtle::Barrier_AtomicIncrement(&unindexed_contains_calls_, 1) !=
       kMaxUnindexedContainsCalls)) {
    return BruteForceContains(p);
  }
  // Otherwise we look up the S2ShapeIndex cell containing this point.  Note
  // the index is built automatically the first time an iterator is created.
  S2ShapeIndex::Iterator it(index_);
  if (!it.Locate(p)) return false;
  return Contains(it, p);
}

bool S2Loop::BruteForceContains(S2Point const& p) const {
  // Empty and full loops don't need a special case, but invalid loops with
  // zero vertices do, so we might as well handle them all at once.
  if (num_vertices() < 3) return origin_inside_;

  S2Point origin = S2::Origin();
  S2EdgeUtil::EdgeCrosser crosser(&origin, &p, &vertex(0));
  bool inside = origin_inside_;
  for (int i = 1; i <= num_vertices(); ++i) {
    inside ^= crosser.EdgeOrVertexCrossing(&vertex(i));
  }
  return inside;
}

bool S2Loop::Contains(S2ShapeIndex::Iterator const& it,
                      S2Point const& p) const {
  // Test containment by drawing a line segment from the cell center to the
  // given point and counting edge crossings.
  S2ClippedShape const& a_clipped = it.cell()->clipped(0);
  bool inside = a_clipped.contains_center();
  int a_num_clipped = a_clipped.num_edges();
  if (a_num_clipped > 0) {
    S2Point center = it.center();
    S2EdgeUtil::EdgeCrosser crosser(&center, &p);
    int ai_prev = -2;
    for (int i = 0; i < a_num_clipped; ++i) {
      int ai = a_clipped.edge(i);
      if (ai != ai_prev + 1) crosser.RestartAt(&vertex(ai));
      ai_prev = ai;
      inside ^= crosser.EdgeOrVertexCrossing(&vertex(ai+1));
    }
  }
  return inside;
}

void S2Loop::Encode(Encoder* const encoder) const {
  encoder->Ensure(num_vertices_ * sizeof(*vertices_) + 20);  // sufficient

  encoder->put8(kCurrentLosslessEncodingVersionNumber);
  encoder->put32(num_vertices_);
  encoder->putn(vertices_, sizeof(*vertices_) * num_vertices_);
  encoder->put8(origin_inside_);
  encoder->put32(depth_);
  DCHECK_GE(encoder->avail(), 0);

  bound_.Encode(encoder);
}

bool S2Loop::Decode(Decoder* const decoder) {
  if (decoder->avail() < sizeof(unsigned char)) return false;
  unsigned char version = decoder->get8();
  switch (version) {
    case kCurrentLosslessEncodingVersionNumber:
      return DecodeInternal(decoder, false);
  }
  return false;
}

bool S2Loop::DecodeWithinScope(Decoder* const decoder) {
  if (decoder->avail() < sizeof(unsigned char)) return false;
  unsigned char version = decoder->get8();
  switch (version) {
    case kCurrentLosslessEncodingVersionNumber:
      return DecodeInternal(decoder, true);
  }
  return false;
}

bool S2Loop::DecodeInternal(Decoder* const decoder,
                            bool within_scope) {
  // Perform all checks before modifying vertex state. Empty loops are
  // explicitly allowed here: a newly created loop has zero vertices
  // and such loops encode and decode properly.
  if (decoder->avail() < sizeof(uint32)) return false;
  uint32 const num_vertices = decoder->get32();
  if (num_vertices > FLAGS_s2polygon_decode_max_num_vertices) {
    return false;
  }
  if (decoder->avail() < (num_vertices * sizeof(*vertices_) +
                          sizeof(uint8) + sizeof(uint32))) {
    return false;
  }
  ResetMutableFields();
  if (owns_vertices_) delete[] vertices_;
  num_vertices_ = num_vertices;

  // x86 can do unaligned floating-point reads; however, many other
  // platforms can not. Do not use the zero-copy version if we are on
  // an architecture that does not support unaligned reads, and the
  // pointer is not correctly aligned.
#if defined(ARCH_PIII) || defined(ARCH_K8)
  bool is_misaligned = false;
#else
  bool is_misaligned = ((intptr_t)decoder->ptr() % sizeof(double) != 0);
#endif
  if (within_scope && !is_misaligned) {
    vertices_ = const_cast<S2Point *>(reinterpret_cast<S2Point const*>(
                    decoder->ptr()));
    decoder->skip(num_vertices_ * sizeof(*vertices_));
    owns_vertices_ = false;
  } else {
    vertices_ = new S2Point[num_vertices_];
    decoder->getn(vertices_, num_vertices_ * sizeof(*vertices_));
    owns_vertices_ = true;
  }
  origin_inside_ = decoder->get8();
  depth_ = decoder->get32();
  if (!bound_.Decode(decoder)) return false;
  subregion_bound_ = S2EdgeUtil::RectBounder::ExpandForSubregions(bound_);

  // An initialized loop will have some non-zero count of vertices. A default
  // (uninitialized) has zero vertices. This code supports encoding and
  // decoding of uninitialized loops, but we only want to call InitIndex for
  // initialized loops. Otherwise we defer InitIndex until the call to Init().
  if (num_vertices > 0) {
    InitIndex();
  }

  return true;
}

// LoopRelation is an abstract class that defines a relationship between two
// loops (Contains, Intersects, or CompareBoundary).
class LoopRelation {
 public:
  LoopRelation() {}
  virtual ~LoopRelation() {}

  // Optionally, a_target() and b_target() can specify an early-exit condition
  // for the loop relation.  If any point P is found such that
  //
  //   A.Contains(P) == a_crossing_target() &&
  //   B.Contains(P) == b_crossing_target()
  //
  // then the loop relation is assumed to be the same as if a pair of crossing
  // edges were found.  For example, the Contains() relation has
  //
  //   a_crossing_target() == 0
  //   b_crossing_target() == 1
  //
  // because if A.Contains(P) == 0 (false) and B.Contains(P) == 1 (true) for
  // any point P, then it is equivalent to finding an edge crossing (i.e.,
  // since Contains() returns false in both cases).
  //
  // Loop relations that do not have an early-exit condition of this form
  // should return -1 for both crossing targets.
  virtual int a_crossing_target() const = 0;
  virtual int b_crossing_target() const = 0;

  // Given a vertex "ab1" that is shared between the two loops, return true if
  // the two associated wedges (a0, ab1, b2) and (b0, ab1, b2) are equivalent
  // to an edge crossing.  The loop relation is also allowed to maintain its
  // own internal state, and can return true if it observes any sequence of
  // wedges that are equivalent to an edge crossing.
  virtual bool WedgesCross(S2Point const& a0, S2Point const& ab1,
                           S2Point const& a2, S2Point const& b0,
                           S2Point const& b2) = 0;
};

// RangeIterator is a wrapper over S2ShapeIndex::Iterator with extra methods
// that are useful for merging the contents of two or more S2ShapeIndexes.
class RangeIterator {
 public:
  // Construct a new RangeIterator positioned at the first cell of the index.
  explicit RangeIterator(S2ShapeIndex const& index)
      : it_(index), end_(S2CellId::End(0)) {
    Refresh();
  }

  // The current S2CellId and cell contents.
  S2CellId id() const { return id_; }
  S2ShapeIndexCell const* cell() const { return it_.cell(); }

  // The min and max leaf cell ids covered by the current cell.  If Done() is
  // true, these methods return a value larger than any valid cell id.
  S2CellId range_min() const { return range_min_; }
  S2CellId range_max() const { return range_max_; }

  // Various other convenience methods for the current cell.
  S2ClippedShape const& clipped() const { return *clipped_; }
  int num_edges() const { return clipped().num_edges(); }
  bool contains_center() const { return clipped().contains_center(); }

  void Next() { it_.Next(); Refresh(); }
  bool Done() { return id_ == end_; }

  // Position the iterator at the first cell that overlaps or follows
  // "target", i.e. such that range_max() >= target.range_min().
  void SeekTo(RangeIterator const& target) {
    it_.Seek(target.range_min());
    // If the current cell does not overlap "target", it is possible that the
    // previous cell is the one we are looking for.  This can only happen when
    // the previous cell contains "target" but has a smaller S2CellId.
    if (it_.Done() || it_.id().range_min() > target.range_max()) {
      it_.Prev();
      if (it_.id().range_max() < target.id()) it_.Next();
    }
    Refresh();
  }

  // Position the iterator at the first cell that follows "target", i.e. the
  // first cell such that range_min() > target.range_max().
  void SeekBeyond(RangeIterator const& target) {
    it_.Seek(target.range_max().next());
    if (!it_.Done() && it_.id().range_min() <= target.range_max()) {
      it_.Next();
    }
    Refresh();
  }

 private:
  // Updates internal state after the iterator has been repositioned.
  void Refresh() {
    if (it_.Done()) {
      id_ = end_;
      clipped_ = NULL;
    } else {
      id_ = it_.id();
      clipped_ = &it_.cell()->clipped(0);
    }
    range_min_ = id_.range_min();
    range_max_ = id_.range_max();
  }

  S2ShapeIndex::Iterator it_;
  S2CellId const end_;
  S2CellId id_, range_min_, range_max_;
  S2ClippedShape const* clipped_;
};

// LoopCrosser is a helper class for determining whether two loops cross.
// It is instantiated twice for each pair of loops to be tested, once for the
// pair (A,B) and once for the pair (B,A), in order to be able to process
// edges in either loop nesting order.
class LoopCrosser {
 public:
  // If "swapped" is true, the loops A and B have been swapped.  This affects
  // how arguments are passed to the given loop relation, since for example
  // A.Contains(B) is not the same as B.Contains(A).
  LoopCrosser(S2Loop const& a, S2Loop const& b,
              LoopRelation* relation, bool swapped)
      : a_(a), b_(b), relation_(relation), swapped_(swapped),
        a_crossing_target_(relation->a_crossing_target()),
        b_crossing_target_(relation->b_crossing_target()),
        b_query_(b.index_) {
    using std::swap;
    if (swapped) swap(a_crossing_target_, b_crossing_target_);
  }

  // Return the crossing targets for the loop relation, taking into account
  // whether the loops have been swapped.
  int a_crossing_target() const { return a_crossing_target_; }
  int b_crossing_target() const { return b_crossing_target_; }

  // Given two iterators positioned such that ai->id().Contains(bi->id()),
  // return true if there is a crossing relationship anywhere within ai->id().
  // Specifically, this method returns true if there is an edge crossing, a
  // wedge crossing, or a point P that matches both "crossing targets".
  // Advances both iterators past ai->id().
  bool HasCrossingRelation(RangeIterator* ai, RangeIterator* bi);

  // Given two index cells, return true if there are any edge crossings or
  // wedge crossings within those cells.
  bool CellCrossesCell(S2ClippedShape const& a_clipped,
                       S2ClippedShape const& b_clipped);

 private:
  // Given two iterators positioned such that ai->id().Contains(bi->id()),
  // return true if there is an edge crossing or wedge crosssing anywhere
  // within ai->id().  Advances "bi" (only) past ai->id().
  bool HasCrossing(RangeIterator* ai, RangeIterator* bi);

  // Given an index cell of A, return true if there are any edge or wedge
  // crossings with any index cell of B contained within "b_id".
  bool CellCrossesAnySubcell(S2ClippedShape const& a_clipped, S2CellId b_id);

  // Prepare to check the given edge of loop A for crossings.
  void StartEdge(int aj);

  // Check the current edge of loop A for crossings with all edges of the
  // given index cell of loop B.
  bool EdgeCrossesCell(S2ClippedShape const& b_clipped);

  S2Loop const& a_;
  S2Loop const& b_;
  LoopRelation* const relation_;
  bool const swapped_;
  int a_crossing_target_, b_crossing_target_;

  // State maintained by StartEdge() and EdgeCrossesCell().
  S2EdgeUtil::EdgeCrosser crosser_;
  int aj_, bj_prev_;

  // Temporary data declared here to avoid repeated memory allocations.
  S2EdgeQuery b_query_;
  vector<S2ShapeIndexCell const*> b_cells_;
};

inline void LoopCrosser::StartEdge(int aj) {
  // Start testing the given edge of A for crossings.
  crosser_.Init(&a_.vertex(aj), &a_.vertex(aj+1));
  aj_ = aj;
  bj_prev_ = -2;
}

inline bool LoopCrosser::EdgeCrossesCell(S2ClippedShape const& b_clipped) {
  // Test the current edge of A against all edges of "b_clipped".
  int b_num_clipped = b_clipped.num_edges();
  for (int j = 0; j < b_num_clipped; ++j) {
    int bj = b_clipped.edge(j);
    if (bj != bj_prev_ + 1) crosser_.RestartAt(&b_.vertex(bj));
    bj_prev_ = bj;
    int crossing = crosser_.RobustCrossing(&b_.vertex(bj+1));
    if (crossing < 0) continue;
    if (crossing > 0) return true;
    // We only need to check each shared vertex once, so we only
    // consider the case where a_vertex(aj_+1) == b_.vertex(bj+1).
    if (a_.vertex(aj_+1) == b_.vertex(bj+1)) {
      if (swapped_ ?
          relation_->WedgesCross(
              b_.vertex(bj), b_.vertex(bj+1), b_.vertex(bj+2),
              a_.vertex(aj_), a_.vertex(aj_+2)) :
          relation_->WedgesCross(
              a_.vertex(aj_), a_.vertex(aj_+1), a_.vertex(aj_+2),
              b_.vertex(bj), b_.vertex(bj+2))) {
        return true;
      }
    }
  }
  return false;
}

bool LoopCrosser::CellCrossesCell(S2ClippedShape const& a_clipped,
                                  S2ClippedShape const& b_clipped) {
  // Test all edges of "a_clipped" against all edges of "b_clipped".
  int a_num_clipped = a_clipped.num_edges();
  for (int i = 0; i < a_num_clipped; ++i) {
    StartEdge(a_clipped.edge(i));
    if (EdgeCrossesCell(b_clipped)) return true;
  }
  return false;
}

bool LoopCrosser::CellCrossesAnySubcell(S2ClippedShape const& a_clipped,
                                        S2CellId b_id) {
  // Test all edges of "a_clipped" against all edges of B.  The relevant B
  // edges are guaranteed to be children of "b_id", which lets us find the
  // correct index cells more efficiently.
  S2PaddedCell b_root(b_id, 0);
  int a_num_clipped = a_clipped.num_edges();
  for (int i = 0; i < a_num_clipped; ++i) {
    int aj = a_clipped.edge(i);
    // Use an S2EdgeQuery starting at "b_root" to find the index cells of B
    // that might contain crossing edges.
    if (!b_query_.GetCells(a_.vertex(aj), a_.vertex(aj+1), b_root, &b_cells_)) {
      continue;
    }
    StartEdge(aj);
    for (int c = 0; c < b_cells_.size(); ++c) {
      if (EdgeCrossesCell(b_cells_[c]->clipped(0))) return true;
    }
  }
  return false;
}

bool LoopCrosser::HasCrossing(RangeIterator* ai, RangeIterator* bi) {
  DCHECK(ai->id().contains(bi->id()));
  // If ai->id() intersects many edges of B, then it is faster to use
  // S2EdgeQuery to narrow down the candidates.  But if it intersects only a
  // few edges, it is faster to check all the crossings directly.  We handle
  // this by advancing "bi" and keeping track of how many edges we would need
  // to test.

  static int const kEdgeQueryMinEdges = 20;  // Tuned using benchmarks.
  int total_edges = 0;
  b_cells_.clear();
  do {
    if (bi->num_edges() > 0) {
      total_edges += bi->num_edges();
      if (total_edges >= kEdgeQueryMinEdges) {
        // There are too many edges to test them directly, so use S2EdgeQuery.
        if (CellCrossesAnySubcell(ai->clipped(), ai->id())) return true;
        bi->SeekBeyond(*ai);
        return false;
      }
      b_cells_.push_back(bi->cell());
    }
    bi->Next();
  } while (bi->id() <= ai->range_max());

  // Test all the edge crossings directly.
  for (int c = 0; c < b_cells_.size(); ++c) {
    if (CellCrossesCell(ai->clipped(), b_cells_[c]->clipped(0))) {
      return true;
    }
  }
  return false;
}

bool LoopCrosser::HasCrossingRelation(RangeIterator* ai, RangeIterator* bi) {
  DCHECK(ai->id().contains(bi->id()));
  if (ai->num_edges() == 0) {
    if (ai->contains_center() == a_crossing_target_) {
      // All points within ai->id() satisfy the crossing target for A, so it's
      // worth iterating through the cells of B to see whether any cell
      // centers also satisfy the crossing target for B.
      do {
        if (bi->contains_center() == b_crossing_target_) return true;
        bi->Next();
      } while (bi->id() <= ai->range_max());
    } else {
      // The crossing target for A is not satisfied, so we skip over the cells
      // of B using binary search.
      bi->SeekBeyond(*ai);
    }
  } else {
    // The current cell of A has at least one edge, so check for crossings.
    if (HasCrossing(ai, bi)) return true;
  }
  ai->Next();
  return false;
}

/*static*/ bool S2Loop::HasCrossingRelation(S2Loop const& a, S2Loop const& b,
                                            LoopRelation* relation) {
  // We look for S2CellId ranges where the indexes of A and B overlap, and
  // then test those edges for crossings.
  RangeIterator ai(a.index_), bi(b.index_);
  LoopCrosser ab(a, b, relation, false);  // Tests edges of A against B
  LoopCrosser ba(b, a, relation, true);   // Tests edges of B against A
  while (!ai.Done() || !bi.Done()) {
    if (ai.range_max() < bi.range_min()) {
      // The A and B cells don't overlap, and A precedes B.
      ai.SeekTo(bi);
    } else if (bi.range_max() < ai.range_min()) {
      // The A and B cells don't overlap, and B precedes A.
      bi.SeekTo(ai);
    } else {
      // One cell contains the other.  Determine which cell is larger.
      int64 ab_relation = ai.id().lsb() - bi.id().lsb();
      if (ab_relation > 0) {
        // A's index cell is larger.
        if (ab.HasCrossingRelation(&ai, &bi)) return true;
      } else if (ab_relation < 0) {
        // B's index cell is larger.
        if (ba.HasCrossingRelation(&bi, &ai)) return true;
      } else {
        // The A and B cells are the same.  Since the two cells have the same
        // center point P, check whether P satisfies the crossing targets.
        if (ai.contains_center() == ab.a_crossing_target() &&
            bi.contains_center() == ab.b_crossing_target()) {
          return true;
        }
        // Otherwise test all the edge crossings directly.
        if (ai.num_edges() > 0 && bi.num_edges() > 0 &&
            ab.CellCrossesCell(ai.clipped(), bi.clipped())) {
          return true;
        }
        ai.Next();
        bi.Next();
      }
    }
  }
  return false;
}

// Loop relation for Contains().
class ContainsRelation : public LoopRelation {
 public:
  ContainsRelation() : found_shared_vertex_(false) {}
  bool found_shared_vertex() const { return found_shared_vertex_; }

  // If A.Contains(P) == false && B.Contains(P) == true, it is equivalent to
  // having an edge crossing (i.e., Contains returns false).
  virtual int a_crossing_target() const { return false; }
  virtual int b_crossing_target() const { return true; }

  virtual bool WedgesCross(S2Point const& a0, S2Point const& ab1,
                           S2Point const& a2, S2Point const& b0,
                           S2Point const& b2) {
    found_shared_vertex_ = true;
    return !S2EdgeUtil::WedgeContains(a0, ab1, a2, b0, b2);
  }

 private:
  bool found_shared_vertex_;
};

bool S2Loop::Contains(S2Loop const* b) const {
  // For this loop A to contains the given loop B, all of the following must
  // be true:
  //
  //  (1) There are no edge crossings between A and B except at vertices.
  //
  //  (2) At every vertex that is shared between A and B, the local edge
  //      ordering implies that A contains B.
  //
  //  (3) If there are no shared vertices, then A must contain a vertex of B
  //      and B must not contain a vertex of A.  (An arbitrary vertex may be
  //      chosen in each case.)
  //
  // The second part of (3) is necessary to detect the case of two loops whose
  // union is the entire sphere, i.e. two loops that contains each other's
  // boundaries but not each other's interiors.
  if (!subregion_bound_.Contains(b->bound_)) return false;

  // Special cases to handle either loop being empty or full.
  if (is_empty_or_full() || b->is_empty_or_full()) {
    return is_full() || b->is_empty();
  }

  // Check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.
  ContainsRelation relation;
  if (HasCrossingRelation(*this, *b, &relation)) return false;

  // There are no crossings, and if there are any shared vertices then A
  // contains B locally at each shared vertex.
  if (relation.found_shared_vertex()) return true;

  // Since there are no edge intersections or shared vertices, we just need to
  // test condition (3) above.  We can skip this test if we discovered that A
  // contains at least one point of B while checking for edge crossings.
  if (!Contains(b->vertex(0))) return false;

  // We still need to check whether (A union B) is the entire sphere.
  // Normally this check is very cheap due to the bounding box precondition.
  if ((b->subregion_bound_.Contains(bound_) ||
       b->bound_.Union(bound_).is_full()) && b->Contains(vertex(0))) {
    return false;
  }
  return true;
}


// Loop relation for Intersects().
class IntersectsRelation : public LoopRelation {
 public:
  IntersectsRelation() : found_shared_vertex_(false) {}
  bool found_shared_vertex() const { return found_shared_vertex_; }

  // If A.Contains(P) == true && B.Contains(P) == true, it is equivalent to
  // having an edge crossing (i.e., Intersects returns true).
  virtual int a_crossing_target() const { return true; }
  virtual int b_crossing_target() const { return true; }

  virtual bool WedgesCross(S2Point const& a0, S2Point const& ab1,
                           S2Point const& a2, S2Point const& b0,
                           S2Point const& b2) {
    found_shared_vertex_ = true;
    return S2EdgeUtil::WedgeIntersects(a0, ab1, a2, b0, b2);
  }

 private:
  bool found_shared_vertex_;
};

bool S2Loop::Intersects(S2Loop const* b) const {
  // a->Intersects(b) if and only if !a->Complement()->Contains(b).
  // This code is similar to Contains(), but is optimized for the case
  // where both loops enclose less than half of the sphere.
  if (!bound_.Intersects(b->bound_)) return false;

  // Check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.
  IntersectsRelation relation;
  if (HasCrossingRelation(*this, *b, &relation)) return true;
  if (relation.found_shared_vertex()) return false;

  // Since there are no edge intersections or shared vertices, the loops
  // intersect only if A contains B, B contains A, or the two loops contain
  // each other's boundaries.  These checks are usually cheap because of the
  // bounding box preconditions.  Note that neither loop is empty (because of
  // the bounding box check above), so it is safe to access vertex(0).

  // Check whether A contains B, or A and B contain each other's boundaries.
  // (Note that A contains all the vertices of B in either case.)
  if (subregion_bound_.Contains(b->bound_) ||
      bound_.Union(b->bound_).is_full()) {
    if (Contains(b->vertex(0))) return true;
  }
  // Check whether B contains A.
  if (b->subregion_bound_.Contains(bound_)) {
    if (b->Contains(vertex(0))) return true;
  }
  return false;
}

// Returns true if the wedge (a0, ab1, a2) contains the "semiwedge" defined as
// any non-empty open set of rays immediately CCW from the edge (ab1, b2).  If
// "reverse_b" is true, then substitute "clockwise" for "CCW"; this simulates
// what would happen if the direction of loop B was reversed.
inline static bool WedgeContainsSemiwedge(S2Point const& a0, S2Point const& ab1,
                                          S2Point const& a2, S2Point const& b2,
                                          bool reverse_b) {
  if (b2 == a0 || b2 == a2) {
    // We have a shared or reversed edge.
    return (b2 == a0) == reverse_b;
  } else {
    return S2::OrderedCCW(a0, a2, b2, ab1);
  }
}

// Loop relation for CompareBoundary().
class CompareBoundaryRelation : public LoopRelation {
 public:
  explicit CompareBoundaryRelation(bool reverse_b):
      reverse_b_(reverse_b), found_shared_vertex_(false),
      contains_edge_(false), excludes_edge_(false) {
  }
  bool found_shared_vertex() const { return found_shared_vertex_; }
  bool contains_edge() const { return contains_edge_; }

  // The CompareBoundary relation does not have a useful early-exit condition,
  // so we return -1 for both crossing targets.
  //
  // Aside: A possible early exit condition could be based on the following.
  //   If A contains a point of both B and ~B, then A intersects Boundary(B).
  //   If ~A contains a point of both B and ~B, then ~A intersects Boundary(B).
  //   So if the intersections of {A, ~A} with {B, ~B} are all non-empty,
  //   the return value is 0, i.e., Boundary(A) intersects Boundary(B).
  // Unfortunately it isn't worth detecting this situation because by the
  // time we have seen a point in all four intersection regions, we are also
  // guaranteed to have seen at least one pair of crossing edges.
  virtual int a_crossing_target() const { return -1; }
  virtual int b_crossing_target() const { return -1; }

  virtual bool WedgesCross(S2Point const& a0, S2Point const& ab1,
                           S2Point const& a2, S2Point const& b0,
                           S2Point const& b2) {
    // Because we don't care about the interior of B, only its boundary, it is
    // sufficient to check whether A contains the semiwedge (ab1, b2).
    found_shared_vertex_ = true;
    if (WedgeContainsSemiwedge(a0, ab1, a2, b2, reverse_b_)) {
      contains_edge_ = true;
    } else {
      excludes_edge_ = true;
    }
    return contains_edge_ & excludes_edge_;
  }

 protected:
  bool const reverse_b_;      // True if loop B should be reversed.
  bool found_shared_vertex_;  // True if any wedge was processed.
  bool contains_edge_;        // True if any edge of B is contained by A.
  bool excludes_edge_;        // True if any edge of B is excluded by A.
};

int S2Loop::CompareBoundary(S2Loop const* b) const {
  DCHECK(!is_empty() && !b->is_empty());
  DCHECK(!b->is_full() || !b->is_hole());

  // The bounds must intersect for containment or crossing.
  if (!bound_.Intersects(b->bound_)) return -1;

  // Full loops are handled as though the loop surrounded the entire sphere.
  if (is_full()) return 1;
  if (b->is_full()) return -1;

  // Check whether there are any edge crossings, and also check the loop
  // relationship at any shared vertices.
  CompareBoundaryRelation relation(b->is_hole());
  if (HasCrossingRelation(*this, *b, &relation)) return 0;
  if (relation.found_shared_vertex()) {
    return relation.contains_edge() ? 1 : -1;
  }

  // There are no edge intersections or shared vertices, so we can check
  // whether A contains an arbitrary vertex of B.
  return Contains(b->vertex(0)) ? 1 : -1;
}

bool S2Loop::ContainsNonCrossingBoundary(S2Loop const* b, bool reverse_b)
    const {
  DCHECK(!is_empty() && !b->is_empty());
  DCHECK(!b->is_full() || !reverse_b);

  // The bounds must intersect for containment.
  if (!bound_.Intersects(b->bound_)) return false;

  // Full loops are handled as though the loop surrounded the entire sphere.
  if (is_full()) return true;
  if (b->is_full()) return false;

  int m = FindVertex(b->vertex(0));
  if (m < 0) {
    // Since vertex b0 is not shared, we can check whether A contains it.
    return Contains(b->vertex(0));
  }
  // Otherwise check whether the edge (b0, b1) is contained by A.
  return WedgeContainsSemiwedge(vertex(m-1), vertex(m), vertex(m+1),
                                b->vertex(1), reverse_b);
}

bool S2Loop::ContainsNested(S2Loop const* b) const {
  if (!subregion_bound_.Contains(b->bound_)) return false;

  // Special cases to handle either loop being empty or full.  Also bail out
  // when B has no vertices to avoid heap overflow on the vertex(1) call
  // below.  (This method is called during polygon initialization before the
  // client has an opportunity to call IsValid().)
  if (is_empty_or_full() || b->num_vertices() < 2) {
    return is_full() || b->is_empty();
  }

  // We are given that A and B do not share any edges, and that either one
  // loop contains the other or they do not intersect.
  int m = FindVertex(b->vertex(1));
  if (m < 0) {
    // Since b->vertex(1) is not shared, we can check whether A contains it.
    return Contains(b->vertex(1));
  }
  // Check whether the edge order around b->vertex(1) is compatible with
  // A containing B.
  return S2EdgeUtil::WedgeContains(vertex(m-1), vertex(m), vertex(m+1),
                                   b->vertex(0), b->vertex(2));
}

bool S2Loop::Equals(S2Loop const* b) const {
  if (num_vertices() != b->num_vertices()) return false;
  for (int i = 0; i < num_vertices(); ++i) {
    if (vertex(i) != b->vertex(i)) return false;
  }
  return true;
}

bool S2Loop::BoundaryEquals(S2Loop const* b) const {
  if (num_vertices() != b->num_vertices()) return false;

  // Special case to handle empty or full loops.  Since they have the same
  // number of vertices, if one loop is empty/full then so is the other.
  if (is_empty_or_full()) return is_empty() == b->is_empty();

  for (int offset = 0; offset < num_vertices(); ++offset) {
    if (vertex(offset) == b->vertex(0)) {
      // There is at most one starting offset since loop vertices are unique.
      for (int i = 0; i < num_vertices(); ++i) {
        if (vertex(i + offset) != b->vertex(i)) return false;
      }
      return true;
    }
  }
  return false;
}

bool S2Loop::BoundaryApproxEquals(S2Loop const* b, double max_error) const {
  if (num_vertices() != b->num_vertices()) return false;

  // Special case to handle empty or full loops.  Since they have the same
  // number of vertices, if one loop is empty/full then so is the other.
  if (is_empty_or_full()) return is_empty() == b->is_empty();

  for (int offset = 0; offset < num_vertices(); ++offset) {
    if (S2::ApproxEquals(vertex(offset), b->vertex(0), max_error)) {
      bool success = true;
      for (int i = 0; i < num_vertices(); ++i) {
        if (!S2::ApproxEquals(vertex(i + offset), b->vertex(i), max_error)) {
          success = false;
          break;
        }
      }
      if (success) return true;
      // Otherwise continue looping.  There may be more than one candidate
      // starting offset since vertices are only matched approximately.
    }
  }
  return false;
}

static bool MatchBoundaries(S2Loop const* a, S2Loop const* b, int a_offset,
                            double max_error) {
  // The state consists of a pair (i,j).  A state transition consists of
  // incrementing either "i" or "j".  "i" can be incremented only if
  // a(i+1+a_offset) is near the edge from b(j) to b(j+1), and a similar rule
  // applies to "j".  The function returns true iff we can proceed all the way
  // around both loops in this way.
  //
  // Note that when "i" and "j" can both be incremented, sometimes only one
  // choice leads to a solution.  We handle this using a stack and
  // backtracking.  We also keep track of which states have already been
  // explored to avoid duplicating work.

  vector<pair<int, int>> pending;
  set<pair<int, int>> done;
  pending.push_back(std::make_pair(0, 0));
  while (!pending.empty()) {
    int i = pending.back().first;
    int j = pending.back().second;
    pending.pop_back();
    if (i == a->num_vertices() && j == b->num_vertices()) {
      return true;
    }
    done.insert(std::make_pair(i, j));

    // If (i == na && offset == na-1) where na == a->num_vertices(), then
    // then (i+1+offset) overflows the [0, 2*na-1] range allowed by vertex().
    // So we reduce the range if necessary.
    int io = i + a_offset;
    if (io >= a->num_vertices()) io -= a->num_vertices();

    if (i < a->num_vertices() && done.count(std::make_pair(i + 1, j)) == 0 &&
        S2EdgeUtil::GetDistance(a->vertex(io + 1), b->vertex(j),
                                b->vertex(j + 1)).radians() <= max_error) {
      pending.push_back(std::make_pair(i + 1, j));
    }
    if (j < b->num_vertices() && done.count(std::make_pair(i, j + 1)) == 0 &&
        S2EdgeUtil::GetDistance(b->vertex(j + 1), a->vertex(io),
                                a->vertex(io + 1)).radians() <= max_error) {
      pending.push_back(std::make_pair(i, j + 1));
    }
  }
  return false;
}

bool S2Loop::BoundaryNear(S2Loop const* b, double max_error) const {
  // Special case to handle empty or full loops.
  if (is_empty_or_full() || b->is_empty_or_full()) {
    return (is_empty() && b->is_empty()) || (is_full() && b->is_full());
  }

  for (int a_offset = 0; a_offset < num_vertices(); ++a_offset) {
    if (MatchBoundaries(this, b, a_offset, max_error)) return true;
  }
  return false;
}

void S2Loop::GetXYZFaceSiTiVertices(S2XYZFaceSiTi* vertices) const {
  for (int i = 0; i < num_vertices(); ++i) {
    vertices[i].xyz = vertex(i);
    vertices[i].cell_level = S2::XYZtoFaceSiTi(vertices[i].xyz,
        &vertices[i].face, &vertices[i].si, &vertices[i].ti);
  }
}

void S2Loop::EncodeCompressed(Encoder* encoder, S2XYZFaceSiTi const* vertices,
                              int snap_level) const {
  // Ensure enough for the data we write before S2EncodePointsCompressed.
  // S2EncodePointsCompressed ensures its space.
  encoder->Ensure(Encoder::kVarintMax32);
  encoder->put_varint32(num_vertices_);

  S2EncodePointsCompressed(vertices, num_vertices_, snap_level, encoder);

  std::bitset<kNumProperties> properties = GetCompressedEncodingProperties();

  // Ensure enough only for what we write.  Let the bound ensure its own
  // space.
  encoder->Ensure(2 * Encoder::kVarintMax32);
  encoder->put_varint32(properties.to_ulong());
  encoder->put_varint32(depth_);
  if (properties.test(kBoundEncoded)) {
    bound_.Encode(encoder);
  }
  DCHECK_GE(encoder->avail(), 0);
}

bool S2Loop::DecodeCompressed(Decoder* decoder, int snap_level) {
  // get_varint32 takes a uint32*, but num_vertices_ is signed.
  // Decode to a temporary variable to avoid reinterpret_cast.
  uint32 unsigned_num_vertices;
  if (!decoder->get_varint32(&unsigned_num_vertices)) {
    return false;
  }
  if (unsigned_num_vertices == 0 ||
      unsigned_num_vertices > FLAGS_s2polygon_decode_max_num_vertices) {
    return false;
  }
  ResetMutableFields();
  if (owns_vertices_) delete[] vertices_;
  num_vertices_ = unsigned_num_vertices;
  vertices_ = new S2Point[num_vertices_];
  owns_vertices_ = true;

  if (!S2DecodePointsCompressed(decoder, num_vertices_, snap_level,
                                vertices_)) {
    return false;
  }
  uint32 properties_uint32;
  if (!decoder->get_varint32(&properties_uint32)) {
    return false;
  }
  std::bitset<kNumProperties> const properties(properties_uint32);
  origin_inside_ = properties.test(kOriginInside);

  uint32 unsigned_depth;
  if (!decoder->get_varint32(&unsigned_depth)) {
    return false;
  }
  depth_ = unsigned_depth;

  if (properties.test(kBoundEncoded)) {
    if (!bound_.Decode(decoder)) {
      return false;
    }
    subregion_bound_ = S2EdgeUtil::RectBounder::ExpandForSubregions(bound_);
  } else {
    InitBound();
  }
  InitIndex();
  return true;
}

std::bitset<kNumProperties> S2Loop::GetCompressedEncodingProperties() const {
  std::bitset<kNumProperties> properties;
  if (origin_inside_) {
    properties.set(kOriginInside);
  }

  // Write whether there is a bound so we can change the threshold later.
  // Recomputing the bound multiplies the decode time taken per vertex
  // by a factor of about 3.5.  Without recomputing the bound, decode
  // takes approximately 125 ns / vertex.  A loop with 63 vertices
  // encoded without the bound will take ~30us to decode, which is
  // acceptable.  At ~3.5 bytes / vertex without the bound, adding
  // the bound will increase the size by <15%, which is also acceptable.
  static const int kMinVerticesForBound = 64;
  if (num_vertices_ >= kMinVerticesForBound) {
    properties.set(kBoundEncoded);
  }
  return properties;
}

/* static */
S2Loop* S2Loop::MakeRegularLoop(S2Point const& center,
                                S1Angle radius,
                                int num_vertices) {
  // TODO(ericv): Unlike the implementation in S2Testing, this version does
  // not support radii of Pi/2 or larger.  Fix this.
  Matrix3x3_d m;
  S2::GetFrame(center, &m);
  vector<S2Point> vertices;
  double radian_step = 2 * M_PI / num_vertices;
  // We create the vertices on the plane tangent to center, so the
  // radius on that plane is larger.
  double planar_radius = tan(radius.radians());
  for (int vi = 0; vi < num_vertices; ++vi) {
    double angle = vi * radian_step;
    S2Point p(planar_radius * cos(angle), planar_radius * sin(angle), 1);
    vertices.push_back(S2::FromFrame(m, p).Normalize());
  }
  return new S2Loop(vertices);
}

