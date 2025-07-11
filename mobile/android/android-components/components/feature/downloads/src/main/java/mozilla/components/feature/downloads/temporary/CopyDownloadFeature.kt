/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.downloads.temporary

import android.content.Context
import androidx.annotation.VisibleForTesting
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import mozilla.components.browser.state.action.BrowserAction
import mozilla.components.browser.state.action.CopyInternetResourceAction
import mozilla.components.browser.state.action.ShareResourceAction
import mozilla.components.browser.state.selector.findTabOrCustomTabOrSelectedTab
import mozilla.components.browser.state.state.content.ShareResourceState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.fetch.Client
import mozilla.components.lib.state.ext.flowScoped
import mozilla.components.support.base.feature.LifecycleAwareFeature
import mozilla.components.support.ktx.android.content.copyImage
import java.util.concurrent.TimeUnit

/**
 * [LifecycleAwareFeature] implementation for copying online resources.
 *
 * This will intercept only [ShareResourceAction] [BrowserAction]s.
 *
 * Following which it will transparently
 *  - download internet resources while respecting the private mode related to cookies handling
 *  - temporarily cache the downloaded resources
 *  - copy the resource to the device clipboard.
 *
 * with a 1 second timeout to ensure a smooth UX.
 *
 * To finish the process in this small timeframe the feature is recommended to be used only for images.
 *
 *  @property context Android context used for various platform interactions.
 *  @property store a reference to the application's [BrowserStore].
 *  @property tabId ID of the tab session, or null if the selected session should be used.
 *  @property onCopyConfirmation The confirmation action of copying an image.
 *  @param httpClient Client used for downloading internet resources.
 *  @param ioDispatcher Coroutine dispatcher used for IO operations like
 *  downloading and the cleanup of old cached files. Defaults to IO.
 */
class CopyDownloadFeature(
    private val context: Context,
    private val store: BrowserStore,
    private val tabId: String?,
    private val onCopyConfirmation: () -> Unit,
    httpClient: Client,
    ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) : TemporaryDownloadFeature(
    context = context,
    httpClient = httpClient,
    ioDispatcher = ioDispatcher,
) {

    /**
     * At most time to allow for the file to be downloaded.
     */
    private val operationTimeoutMs by lazy { TimeUnit.MINUTES.toMinutes(1) }

    override fun start() {
        scope = store.flowScoped { flow ->
            flow.mapNotNull { state -> state.findTabOrCustomTabOrSelectedTab(tabId) }
                .distinctUntilChangedBy { it.content.copy }
                .collect { state ->
                    state.content.copy?.let { copyState ->
                        logger.debug("Starting the copying process")
                        startCopy(copyState)

                        // This is a fire and forget action, not something that we want lingering the tab state.
                        store.dispatch(CopyInternetResourceAction.ConsumeCopyAction(state.id))
                    }
                }
        }
    }

    @VisibleForTesting
    internal fun startCopy(internetResource: ShareResourceState.InternetResource) {
        val coroutineExceptionHandler = coroutineExceptionHandler("Copy")

        scope?.launch(coroutineExceptionHandler) {
            withTimeout(operationTimeoutMs) {
                val download = download(internetResource)
                copy(download.canonicalPath, onCopyConfirmation)
            }
        }
    }

    @VisibleForTesting
    internal fun copy(filePath: String, onCopyConfirmation: () -> Unit) =
        context.copyImage(filePath, onCopyConfirmation)
}
