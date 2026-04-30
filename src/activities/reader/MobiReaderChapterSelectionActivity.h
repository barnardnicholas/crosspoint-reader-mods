#pragma once

#include <Mobi.h>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class MobiReaderChapterSelectionActivity final : public Activity {
  // Raw non-owning pointer — MobiReaderActivity owns the Mobi and outlives this activity on the stack
  const Mobi* mobi;
  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;

  int getPageItems() const;
  int getTotalItems() const;

 public:
  explicit MobiReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const Mobi* mobi, int currentChapterIndex)
      : Activity("MobiReaderChapterSelection", renderer, mappedInput),
        mobi(mobi),
        selectorIndex(currentChapterIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
