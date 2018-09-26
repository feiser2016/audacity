/**********************************************************************

Audacity: A Digital Audio Editor

TimeShiftHandle.cpp

Paul Licameli split from TrackPanel.cpp

**********************************************************************/

#include "../../Audacity.h"
#include "TimeShiftHandle.h"
#include "../../Experimental.h"

#include "TrackControls.h"
#include "../../AColor.h"
#include "../../HitTestResult.h"
#include "../../Project.h"
#include "../../RefreshCode.h"
#include "../../TrackPanelMouseEvent.h"
#include "../../toolbars/ToolsToolBar.h"
#include "../../UndoManager.h"
#include "../../WaveTrack.h"
#include "../../../images/Cursors.h"

TimeShiftHandle::TimeShiftHandle
( const std::shared_ptr<Track> &pTrack, bool gripHit )
   : mCapturedTrack{ pTrack }
   , mGripHit{ gripHit }
{
}

void TimeShiftHandle::Enter(bool)
{
#ifdef EXPERIMENTAL_TRACK_PANEL_HIGHLIGHTING
   mChangeHighlight = RefreshCode::RefreshCell;
#endif
}

HitTestPreview TimeShiftHandle::HitPreview
(const AudacityProject *WXUNUSED(pProject), bool unsafe)
{
   static auto disabledCursor =
      ::MakeCursor(wxCURSOR_NO_ENTRY, DisabledCursorXpm, 16, 16);
   static auto slideCursor =
      MakeCursor(wxCURSOR_SIZEWE, TimeCursorXpm, 16, 16);
   // TODO: Should it say "track or clip" ?  Non-wave tracks can move, or clips in a wave track.
   // TODO: mention effects of shift (move all clips of selected wave track) and ctrl (move vertically only) ?
   //  -- but not all of that is available in multi tool.
   auto message = _("Click and drag to move a track in time");

   return {
      message,
      (unsafe
       ? &*disabledCursor
       : &*slideCursor)
   };
}

UIHandlePtr TimeShiftHandle::HitAnywhere
(std::weak_ptr<TimeShiftHandle> &holder,
 const std::shared_ptr<Track> &pTrack, bool gripHit)
{
   auto result = std::make_shared<TimeShiftHandle>( pTrack, gripHit );
   result = AssignUIHandlePtr(holder, result);
   return result;
}

UIHandlePtr TimeShiftHandle::HitTest
(std::weak_ptr<TimeShiftHandle> &holder,
 const wxMouseState &state, const wxRect &rect,
 const std::shared_ptr<Track> &pTrack)
{
   /// method that tells us if the mouse event landed on a
   /// time-slider that allows us to time shift the sequence.
   /// (Those are the two "grips" drawn at left and right edges for multi tool mode.)

   // Perhaps we should delegate this to TrackArtist as only TrackArtist
   // knows what the real sizes are??

   // The drag Handle width includes border, width and a little extra margin.
   const int adjustedDragHandleWidth = 14;
   // The hotspot for the cursor isn't at its centre.  Adjust for this.
   const int hotspotOffset = 5;

   // We are doing an approximate test here - is the mouse in the right or left border?
   if (!(state.m_x + hotspotOffset < rect.x + adjustedDragHandleWidth ||
       state.m_x + hotspotOffset >= rect.x + rect.width - adjustedDragHandleWidth))
      return {};

   return HitAnywhere( holder, pTrack, true );
}

TimeShiftHandle::~TimeShiftHandle()
{
}

namespace
{
   // Adds a track's clips to state.capturedClipArray within a specified time
   void AddClipsToCaptured
      ( ClipMoveState &state, Track *t, double t0, double t1 )
   {
      auto &clips = state.capturedClipArray;

      bool exclude = true;
      if (t->GetKind() == Track::Wave)
      {
         exclude = false;
         for(const auto &clip: static_cast<WaveTrack*>(t)->GetClips())
            if ( ! clip->AfterClip(t0) && ! clip->BeforeClip(t1) &&
               // Avoid getting clips that were already captured
                 ! std::any_of( clips.begin(), clips.end(),
                    [&](const TrackClip &c) { return c.clip == clip.get(); } ) )
               clips.emplace_back( t, clip.get() );
      }
      else
      {
         // This handles label tracks rather heavy-handedly -- it would be nice to
         // treat individual labels like clips

         // Avoid adding a track twice
         if( ! std::any_of( clips.begin(), clips.end(),
            [&](const TrackClip &c) { return c.track == t; } ) ) {
   #ifdef USE_MIDI
            // do not add NoteTrack if the data is outside of time bounds
            if (t->GetKind() == Track::Note) {
               if (t->GetEndTime() < t0 || t->GetStartTime() > t1)
                  return;
            }
   #endif
            clips.emplace_back( t, nullptr );
         }
      }
      if ( exclude )
         state.trackExclusions.push_back(t);
   }

   // Helper for the above, adds a track's clips to capturedClipArray (eliminates
   // duplication of this logic)
   void AddClipsToCaptured
      ( ClipMoveState &state, const ViewInfo &viewInfo,
        Track *t, bool withinSelection )
   {
      if (withinSelection)
         AddClipsToCaptured( state, t, viewInfo.selectedRegion.t0(),
                            viewInfo.selectedRegion.t1() );
      else
         AddClipsToCaptured( state, t, t->GetStartTime(), t->GetEndTime() );
   }

   // Don't count right channels.
   WaveTrack *NthAudioTrack(TrackList &list, int nn)
   {
      if (nn >= 0) {
         TrackListOfKindIterator iter(Track::Wave, &list);
         Track *pTrack = iter.First();
         while (pTrack && nn--)
            pTrack = iter.Next(true);
         return static_cast<WaveTrack*>(pTrack);
      }

      return NULL;
   }

   // Don't count right channels.
   int TrackPosition(TrackList &list, Track *pFindTrack)
   {
      Track *const partner = pFindTrack->GetLink();
      TrackListOfKindIterator iter(Track::Wave, &list);
      int nn = 0;
      for (Track *pTrack = iter.First(); pTrack; pTrack = iter.Next(true), ++nn) {
         if (pTrack == pFindTrack ||
             pTrack == partner)
            return nn;
      }
      return -1;
   }

   WaveClip *FindClipAtTime(WaveTrack *pTrack, double time)
   {
      if (pTrack) {
         // WaveClip::GetClipAtX doesn't work unless the clip is on the screen and can return bad info otherwise
         // instead calculate the time manually
         double rate = pTrack->GetRate();
         auto s0 = (sampleCount)(time * rate + 0.5);

         if (s0 >= 0)
            return pTrack->GetClipAtSample(s0);
      }

      return 0;
   }

   void DoOffset( ClipMoveState &state, Track *pTrack, double offset,
                  WaveClip *pExcludedClip = nullptr )
   {
      auto &clips = state.capturedClipArray;
      if ( !clips.empty() ) {
         for (auto &clip : clips) {
            if (clip.clip) {
               if (clip.clip != pExcludedClip)
                  clip.clip->Offset( offset );
            }
            else
               clip.track->Offset( offset );
         }
      }
      else if ( pTrack ) {
         // Was a shift-click
         for (auto channel =
                 pTrack->GetLink() && !pTrack->GetLinked()
                    ? pTrack->GetLink() : pTrack;
              channel;
              channel = channel->GetLinked() ? channel->GetLink() : nullptr)
            channel->Offset( offset );
      }
   }
}

void TimeShiftHandle::CreateListOfCapturedClips
   ( ClipMoveState &state, const ViewInfo &viewInfo, Track &capturedTrack,
     TrackList &trackList, bool syncLocked, double clickTime )
{
// The captured clip is the focus, but we need to create a list
   // of all clips that have to move, also...

   state.capturedClipArray.clear();

   // First, if click was in selection, capture selected clips; otherwise
   // just the clicked-on clip
   if ( state.capturedClipIsSelection ) {
      TrackListIterator iter( &trackList );
      for (Track *t = iter.First(); t; t = iter.Next()) {
         if (t->GetSelected())
            AddClipsToCaptured( state, viewInfo, t, true );
      }
   }
   else {
      state.capturedClipArray.push_back
         (TrackClip( &capturedTrack, state.capturedClip ));

      // Check for stereo partner
      Track *partner = capturedTrack.GetLink();
      WaveTrack *wt;
      if (state.capturedClip &&
            // Assume linked track is wave or null
            nullptr != (wt = static_cast<WaveTrack*>(partner))) {
         WaveClip *const clip = FindClipAtTime(wt, clickTime);

         if (clip)
            state.capturedClipArray.push_back(TrackClip(partner, clip));
      }
   }

   // Now, if sync-lock is enabled, capture any clip that's linked to a
   // captured clip.
   if ( syncLocked ) {
      // AWD: capturedClipArray expands as the loop runs, so newly-added
      // clips are considered (the effect is like recursion and terminates
      // because AddClipsToCaptured doesn't add duplicate clips); to remove
      // this behavior just store the array size beforehand.
      for (unsigned int i = 0; i < state.capturedClipArray.size(); ++i) {
         auto &trackClip = state.capturedClipArray[i];
         // Capture based on tracks that have clips -- that means we
         // don't capture based on links to label tracks for now (until
         // we can treat individual labels as clips)
         if ( trackClip.clip ) {
            // Iterate over sync-lock group tracks.
            SyncLockedTracksIterator git( &trackList );
            for (Track *t = git.StartWith( trackClip.track  );
                  t; t = git.Next() )
               AddClipsToCaptured(state, t,
                     trackClip.clip->GetStartTime(),
                     trackClip.clip->GetEndTime() );
         }
#ifdef USE_MIDI
         // Capture additional clips from NoteTracks
         Track *nt = trackClip.track;
         if (nt->GetKind() == Track::Note) {
            // Iterate over sync-lock group tracks.
            SyncLockedTracksIterator git( &trackList );
            for (Track *t = git.StartWith(nt); t; t = git.Next())
               AddClipsToCaptured
                  ( state, t, nt->GetStartTime(), nt->GetEndTime() );
         }
#endif
      }
   }
}

void TimeShiftHandle::DoSlideHorizontal
   ( ClipMoveState &state, TrackList &trackList, Track &capturedTrack )
{
   // Given a signed slide distance, move clips, but subject to constraint of
   // non-overlapping with other clips, so the distance may be adjusted toward
   // zero.
   if ( state.capturedClipArray.size() )
   {
      double allowed;
      double initialAllowed;
      double safeBigDistance = 1000 + 2.0 * ( trackList.GetEndTime() -
                                              trackList.GetStartTime() );

      do { // loop to compute allowed, does not actually move anything yet
         initialAllowed = state.hSlideAmount;

         for ( auto &trackClip : state.capturedClipArray ) {
            if (const auto clip = trackClip.clip) {
               // only audio clips are used to compute allowed
               const auto track = static_cast<WaveTrack *>( trackClip.track );

               // Move all other selected clips totally out of the way
               // temporarily because they're all moving together and
               // we want to find out if OTHER clips are in the way,
               // not one of the moving ones
               DoOffset( state, nullptr, -safeBigDistance, clip );
               auto cleanup = finally( [&]
                  { DoOffset( state, nullptr, safeBigDistance, clip ); } );

               if ( track->CanOffsetClip(clip, state.hSlideAmount, &allowed) ) {
                  if ( state.hSlideAmount != allowed ) {
                     state.hSlideAmount = allowed;
                     state.snapLeft = state.snapRight = -1; // see bug 1067
                  }
               }
               else {
                  state.hSlideAmount = 0.0;
                  state.snapLeft = state.snapRight = -1; // see bug 1067
               }
            }
         }
      } while ( state.hSlideAmount != initialAllowed );

      // finally, here is where clips are moved
      if ( state.hSlideAmount != 0.0 )
         DoOffset( state, nullptr, state.hSlideAmount );
   }
   else
      // For Shift key down, or
      // For non wavetracks, specifically label tracks ...
      DoOffset( state, &capturedTrack, state.hSlideAmount );
}

UIHandle::Result TimeShiftHandle::Click
(const TrackPanelMouseEvent &evt, AudacityProject *pProject)
{
   using namespace RefreshCode;
   const bool unsafe = pProject->IsAudioActive();
   if ( unsafe )
      return Cancelled;

   const wxMouseEvent &event = evt.event;
   const wxRect &rect = evt.rect;
   const ViewInfo &viewInfo = pProject->GetViewInfo();

   const auto pTrack = std::static_pointer_cast<Track>(evt.pCell);
   if (!pTrack)
      return RefreshCode::Cancelled;

   TrackList *const trackList = pProject->GetTracks();

   mClipMoveState.clear();
   mDidSlideVertically = false;

   ToolsToolBar *const ttb = pProject->GetToolsToolBar();
   const bool multiToolModeActive = (ttb && ttb->IsDown(multiTool));

   const double clickTime =
      viewInfo.PositionToTime(event.m_x, rect.x);
   mClipMoveState.capturedClipIsSelection =
      (pTrack->GetSelected() &&
       clickTime >= viewInfo.selectedRegion.t0() &&
       clickTime < viewInfo.selectedRegion.t1());

   WaveTrack *wt = pTrack->GetKind() == Track::Wave
      ? static_cast<WaveTrack*>(pTrack.get()) : nullptr;

   if ((wt
#ifdef USE_MIDI
      || pTrack->GetKind() == Track::Note
#endif
      ) && !event.ShiftDown())
   {
#ifdef USE_MIDI
      if (!wt)
         mClipMoveState.capturedClip = nullptr;
      else
#endif
      {
         mClipMoveState.capturedClip = wt->GetClipAtX(event.m_x);
         if (mClipMoveState.capturedClip == NULL)
            return Cancelled;
      }

      CreateListOfCapturedClips
         ( mClipMoveState, viewInfo, *pTrack, *trackList,
           pProject->IsSyncLocked(), clickTime );
   }
   else {
      // Shift was down, or track was not Wave or Note
      mClipMoveState.capturedClip = NULL;
      mClipMoveState.capturedClipArray.clear();
   }

   mSlideUpDownOnly = event.CmdDown() && !multiToolModeActive;
   mRect = rect;
   mClipMoveState.mMouseClickX = event.m_x;
   mSnapManager = std::make_shared<SnapManager>(trackList,
                                  &viewInfo,
                                  &mClipMoveState.capturedClipArray,
                                  &mClipMoveState.trackExclusions,
                                  true); // don't snap to time
   mClipMoveState.snapLeft = -1;
   mClipMoveState.snapRight = -1;
   mSnapPreferRightEdge =
      mClipMoveState.capturedClip &&
      (fabs(clickTime - mClipMoveState.capturedClip->GetEndTime()) <
       fabs(clickTime - mClipMoveState.capturedClip->GetStartTime()));

   return RefreshNone;
}

namespace {
   double FindDesiredSlideAmount(
      const ViewInfo &viewInfo, wxCoord xx, const wxMouseEvent &event,
      SnapManager *pSnapManager,
      bool slideUpDownOnly, bool snapPreferRightEdge,
      ClipMoveState &state,
      Track &capturedTrack, Track &track )
   {
      if (slideUpDownOnly)
         return 0.0;
      else {
         double desiredSlideAmount =
            viewInfo.PositionToTime(event.m_x) -
            viewInfo.PositionToTime(state.mMouseClickX);
         double clipLeft = 0, clipRight = 0;

         if (track.GetKind() == Track::Wave) {
            WaveTrack *const mtw = static_cast<WaveTrack*>(&track);
            const double rate = mtw->GetRate();
            // set it to a sample point
            desiredSlideAmount = rint(desiredSlideAmount * rate) / rate;
         }

         // Adjust desiredSlideAmount using SnapManager
         if (pSnapManager) {
            if (state.capturedClip) {
               clipLeft = state.capturedClip->GetStartTime()
                  + desiredSlideAmount;
               clipRight = state.capturedClip->GetEndTime()
                  + desiredSlideAmount;
            }
            else {
               clipLeft = capturedTrack.GetStartTime() + desiredSlideAmount;
               clipRight = capturedTrack.GetEndTime() + desiredSlideAmount;
            }

            auto results =
               pSnapManager->Snap(&capturedTrack, clipLeft, false);
            auto newClipLeft = results.outTime;
            results =
               pSnapManager->Snap(&capturedTrack, clipRight, false);
            auto newClipRight = results.outTime;

            // Only one of them is allowed to snap
            if (newClipLeft != clipLeft && newClipRight != clipRight) {
               // Un-snap the un-preferred edge
               if (snapPreferRightEdge)
                  newClipLeft = clipLeft;
               else
                  newClipRight = clipRight;
            }

            // Take whichever one snapped (if any) and compute the NEW desiredSlideAmount
            state.snapLeft = -1;
            state.snapRight = -1;
            if (newClipLeft != clipLeft) {
               const double difference = (newClipLeft - clipLeft);
               desiredSlideAmount += difference;
               state.snapLeft =
                  viewInfo.TimeToPosition(newClipLeft, xx);
            }
            else if (newClipRight != clipRight) {
               const double difference = (newClipRight - clipRight);
               desiredSlideAmount += difference;
               state.snapRight =
                  viewInfo.TimeToPosition(newClipRight, xx);
            }
         }
         return desiredSlideAmount;
      }
   }

   struct TemporaryClipRemover {
      TemporaryClipRemover( ClipMoveState &clipMoveState )
         : state( clipMoveState )
      {
         // Pluck the moving clips out of their tracks
         for ( auto &trackClip : state.capturedClipArray ) {
            WaveClip *const pSrcClip = trackClip.clip;
            if (pSrcClip)
               trackClip.holder =
                  // Assume track is wave because it has a clip
                  static_cast<WaveTrack*>(trackClip.track)->
                     RemoveAndReturnClip(pSrcClip);
         }
      }

      void Fail()
      {
         // Cause destructor to put all clips back where they came from
         for ( auto &trackClip : state.capturedClipArray )
            trackClip.dstTrack = static_cast<WaveTrack*>(trackClip.track);
      }

      ~TemporaryClipRemover()
      {
         // Complete (or roll back) the vertical move.
         // Put moving clips into their destination tracks
         // which become the source tracks when we move again
         for ( auto &trackClip : state.capturedClipArray ) {
            WaveClip *const pSrcClip = trackClip.clip;
            if (pSrcClip) {
               const auto dstTrack = trackClip.dstTrack;
               dstTrack->AddClip(std::move(trackClip.holder));
               trackClip.track = dstTrack;
            }
         }
      }

      ClipMoveState &state;
   };
}

UIHandle::Result TimeShiftHandle::Drag
(const TrackPanelMouseEvent &evt, AudacityProject *pProject)
{
   using namespace RefreshCode;
   const bool unsafe = pProject->IsAudioActive();
   if (unsafe) {
      this->Cancel(pProject);
      return RefreshAll | Cancelled;
   }

   const wxMouseEvent &event = evt.event;
   ViewInfo &viewInfo = pProject->GetViewInfo();

   Track *track = dynamic_cast<Track*>(evt.pCell.get());

   // Uncommenting this permits drag to continue to work even over the controls area
   /*
   track = static_cast<CommonTrackPanelCell*>(evt.pCell)->FindTrack().get();
   */

   if (!track) {
      // Allow sliding if the pointer is not over any track, but only if x is
      // within the bounds of the tracks area.
      if (event.m_x >= mRect.GetX() &&
         event.m_x < mRect.GetX() + mRect.GetWidth())
          track = mCapturedTrack.get();
   }

   // May need a shared_ptr to reassign mCapturedTrack below
   auto pTrack = Track::Pointer( track );
   if (!pTrack)
      return RefreshCode::RefreshNone;


   TrackList *const trackList = pProject->GetTracks();

   // GM: DoSlide now implementing snap-to
   // samples functionality based on sample rate.

   // Start by undoing the current slide amount; everything
   // happens relative to the original horizontal position of
   // each clip...
   DoOffset(
      mClipMoveState, mCapturedTrack.get(), -mClipMoveState.hSlideAmount );

   if ( mClipMoveState.capturedClipIsSelection ) {
      // Slide the selection, too
      viewInfo.selectedRegion.move( -mClipMoveState.hSlideAmount );
   }
   mClipMoveState.hSlideAmount = 0.0;

   double desiredSlideAmount =
      FindDesiredSlideAmount( viewInfo, mRect.x, event, mSnapManager.get(),
         mSlideUpDownOnly, mSnapPreferRightEdge, mClipMoveState,
         *mCapturedTrack, *pTrack );

   // Scroll during vertical drag.
   // EnsureVisible(pTrack); //vvv Gale says this has problems on Linux, per bug 393 thread. Revert for 2.0.2.
   bool slidVertically = false;

   // If the mouse is over a track that isn't the captured track,
   // decide which tracks the captured clips should go to.
   if (mClipMoveState.capturedClip &&
       pTrack != mCapturedTrack &&
       pTrack->GetKind() == Track::Wave
       /* && !mCapturedClipIsSelection*/)
   {
      const int diff =
         TrackPosition(*trackList, pTrack.get()) -
         TrackPosition(*trackList, mCapturedTrack.get());
      for ( auto &trackClip : mClipMoveState.capturedClipArray ) {
         if (trackClip.clip) {
            // Move all clips up or down by an equal count of audio tracks.
            Track *const pSrcTrack = trackClip.track;
            auto pDstTrack = NthAudioTrack(*trackList,
               diff + TrackPosition(*trackList, pSrcTrack));
            // Can only move mono to mono, or left to left, or right to right
            // And that must be so for each captured clip
            bool stereo = (pSrcTrack->GetLink() != 0);
            if (pDstTrack && stereo && !pSrcTrack->GetLinked())
               // Assume linked track is wave or null
               pDstTrack = static_cast<WaveTrack*>(pDstTrack->GetLink());
            bool ok = pDstTrack &&
            (stereo == (pDstTrack->GetLink() != 0)) &&
            (!stereo || (pSrcTrack->GetLinked() == pDstTrack->GetLinked()));
            if (ok)
               trackClip.dstTrack = pDstTrack;
            else
               return RefreshAll;
         }
      }

      // Having passed that test, remove clips temporarily from their
      // tracks, so moving clips don't interfere with each other
      // when we call CanInsertClip()
      TemporaryClipRemover remover( mClipMoveState );

      // Now check that the move is possible
      bool ok = true;
      // The test for tolerance will need review with FishEye!
      // The tolerance is supposed to be the time for one pixel, i.e. one pixel tolerance 
      // at current zoom.
      double slide = desiredSlideAmount; // remember amount requested.
      double tolerance = viewInfo.PositionToTime(event.m_x+1) - viewInfo.PositionToTime(event.m_x);

      // The desiredSlideAmount may change and the tolerance may get used up.
      for ( auto &trackClip : mClipMoveState.capturedClipArray ) {
         WaveClip *const pSrcClip = trackClip.clip;
         if (pSrcClip) {
            ok = trackClip.dstTrack->CanInsertClip(
               pSrcClip, desiredSlideAmount, tolerance );
            if( !ok  )
               break;
         }
      }

      if( ok ) {
         // fits ok, but desiredSlideAmount could have been updated to get the clip to fit.
         // Check again, in the new position, this time with zero tolerance.
         tolerance = 0.0;
         for ( auto &trackClip : mClipMoveState.capturedClipArray ) {
            WaveClip *const pSrcClip = trackClip.clip;
            if (pSrcClip) {
               ok = trackClip.dstTrack->CanInsertClip(
                  pSrcClip, desiredSlideAmount, tolerance);
               if ( !ok )
                  break;
            }
         }
      }

      if (!ok) {
         // Failure, even with using tolerance.
         remover.Fail();

         // Failure -- we'll put clips back where they were
         // ok will next indicate if a horizontal slide is OK.
         ok = true; // assume slide is OK.
         tolerance = 0.0;
         desiredSlideAmount = slide;
         for ( auto &trackClip : mClipMoveState.capturedClipArray ) {
            WaveClip *const pSrcClip = trackClip.clip;
            if (pSrcClip){
               // back to the track it came from...
               trackClip.dstTrack = static_cast<WaveTrack*>(trackClip.track);
               ok = ok && trackClip.dstTrack->CanInsertClip(pSrcClip, desiredSlideAmount, tolerance);
            }
         }
         for ( auto &trackClip : mClipMoveState.capturedClipArray)  {
            WaveClip *const pSrcClip = trackClip.clip;
            if (pSrcClip){

               // Attempt to move to a new track did not work.
               // Put the clip back appropriately shifted!
               if( ok) 
                  trackClip.holder->Offset(slide);
            }
         }
         // Make the offset permanent; start from a "clean slate"
         if( ok ) {
            mClipMoveState.mMouseClickX = event.m_x;
            if (mClipMoveState.capturedClipIsSelection) {
               // Slide the selection, too
               viewInfo.selectedRegion.move( slide );
            }
            mClipMoveState.hSlideAmount = 0;
         }

         return RefreshAll;
      }
      else {
         mCapturedTrack = pTrack;
         mDidSlideVertically = true;

         // Make the offset permanent; start from a "clean slate"
         mClipMoveState.mMouseClickX = event.m_x;
      }

      // Not done yet, check for horizontal movement.
      slidVertically = true;
   }

   if (desiredSlideAmount == 0.0)
      return RefreshAll;

   mClipMoveState.hSlideAmount = desiredSlideAmount;

   DoSlideHorizontal( mClipMoveState, *trackList, *mCapturedTrack );

   if (mClipMoveState.capturedClipIsSelection) {
      // Slide the selection, too
      viewInfo.selectedRegion.move( mClipMoveState.hSlideAmount );
   }

   if (slidVertically) {
      // NEW origin
      mClipMoveState.hSlideAmount = 0;
   }

   return RefreshAll;
}

HitTestPreview TimeShiftHandle::Preview
(const TrackPanelMouseState &, const AudacityProject *pProject)
{
   // After all that, it still may be unsafe to drag.
   // Even if so, make an informative cursor change from default to "banned."
   const bool unsafe = pProject->IsAudioActive();
   return HitPreview(pProject, unsafe);
}

UIHandle::Result TimeShiftHandle::Release
(const TrackPanelMouseEvent &, AudacityProject *pProject,
 wxWindow *)
{
   using namespace RefreshCode;
   const bool unsafe = pProject->IsAudioActive();
   if (unsafe)
      return this->Cancel(pProject);

   Result result = RefreshNone;

   // Do not draw yellow lines
   if ( mClipMoveState.snapLeft != -1 || mClipMoveState.snapRight != -1) {
      mClipMoveState.snapLeft = mClipMoveState.snapRight = -1;
      result |= RefreshAll;
   }

   if ( !mDidSlideVertically && mClipMoveState.hSlideAmount == 0 )
      return result;
   
   for ( auto &trackClip : mClipMoveState.capturedClipArray )
   {
      WaveClip* pWaveClip = trackClip.clip;
      // Note that per AddClipsToCaptured(Track *t, double t0, double t1),
      // in the non-WaveTrack case, the code adds a NULL clip to capturedClipArray,
      // so we have to check for that any time we're going to deref it.
      // Previous code did not check it here, and that caused bug 367 crash.
      if (pWaveClip &&
         trackClip.track != trackClip.origTrack)
      {
         // Now that user has dropped the clip into a different track,
         // make sure the sample rate matches the destination track.
         // Assume the clip was dropped in a wave track
         pWaveClip->Resample
            (static_cast<WaveTrack*>(trackClip.track)->GetRate());
         pWaveClip->MarkChanged();
      }
   }

   wxString msg;
   bool consolidate;
   if (mDidSlideVertically) {
      msg = _("Moved clips to another track");
      consolidate = false;
   }
   else {
      msg.Printf(
         ( mClipMoveState.hSlideAmount > 0
           ? _("Time shifted tracks/clips right %.02f seconds")
           : _("Time shifted tracks/clips left %.02f seconds") ),
         fabs( mClipMoveState.hSlideAmount ) );
      consolidate = true;
   }
   pProject->PushState(msg, _("Time-Shift"),
      consolidate ? (UndoPush::CONSOLIDATE) : (UndoPush::AUTOSAVE));

   return result | FixScrollbars;
}

UIHandle::Result TimeShiftHandle::Cancel(AudacityProject *pProject)
{
   pProject->RollbackState();
   return RefreshCode::RefreshAll;
}

void TimeShiftHandle::DrawExtras
(DrawingPass pass,
 wxDC * dc, const wxRegion &, const wxRect &)
{
   if (pass == Panel) {
      // Draw snap guidelines if we have any
      if ( mSnapManager )
         mSnapManager->Draw
            ( dc, mClipMoveState.snapLeft, mClipMoveState.snapRight );
   }
}
