// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_SELECT_CONTENT_ORDERBYHEIGHTPROVIDER_H_
#define SCANTAILOR_SELECT_CONTENT_ORDERBYHEIGHTPROVIDER_H_

#include "PageOrderProvider.h"
#include "Settings.h"
#include "intrusive_ptr.h"

namespace select_content {
class OrderByHeightProvider : public PageOrderProvider {
 public:
  explicit OrderByHeightProvider(intrusive_ptr<Settings> settings);

  bool precedes(const PageId& lhsPage, bool lhsIncomplete, const PageId& rhsPage, bool rhsIncomplete) const override;

 private:
  intrusive_ptr<Settings> m_settings;
};
}  // namespace select_content
#endif
