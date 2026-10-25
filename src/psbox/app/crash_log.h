// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace xc::app {

// Prima di aprire xcloud.log: il file della sessione appena conclusa diventa
// xcloud.prev.log. È quello che «Invia log» manda. Non invia nulla.
bool preparePreviousLog();

// POST manuale di xcloud.prev.log (ultime 200 KB). Avviso: «Log inviato» o
// «Invio fallito». Se manca PSBOX_WEBHOOK_KEY l'invio non parte.
void sendPreviousLog();

}  // namespace xc::app
