// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#ifndef SCANTAILOR_CORE_ABSTRACTCOMMAND_H_
#define SCANTAILOR_CORE_ABSTRACTCOMMAND_H_

#include "intrusive_ptr.h"
#include "ref_countable.h"

template <typename Res, typename... ArgTypes>
class AbstractCommand : public ref_countable {
 public:
  using Ptr = intrusive_ptr<AbstractCommand>;

  virtual Res operator()(ArgTypes... args) = 0;
};

#endif  // ifndef SCANTAILOR_CORE_ABSTRACTCOMMAND_H_
