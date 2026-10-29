/* SPDX-License-Identifier: GPL-2.0-or-later WITH GStreamer-exception-2008
 * SPDX-FileCopyrightText: 2011 Red Hat, Inc.
 *
 * Authors: Cosimo Cecchi <cosimoc@redhat.com>
 */

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import GioUnix from 'gi://GioUnix';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import PapersDocument from 'gi://PapersDocument';
import PapersView from 'gi://PapersView';
// eslint-disable-next-line no-restricted-properties
const Format = imports.format;

import {Renderer} from '../core/renderer.js';
import * as Image from './image.js';
import {setupActions} from '../util/action.js';
import {Connection} from '../util/connection.js';

let papersInitialized = false;

export const Klass = class PdfRenderer extends Adw.Bin {
    static {
        GObject.registerClass({
            Implements: [Renderer],
            Template: 'resource:///org/gnome/NautilusPreviewer/ui/pdf.ui',
            InternalChildren: [
                'model', 'view', 'toolbarBack', 'toolbarForward', 'pageLabel',
                'toolbar', 'annotationsContext', 'searchContext',
            ],
        }, this);
    }

    #loadJobId = new Connection('finished', job => this._onLoadJobFinished(job));

    constructor(file, fileInfo, constructProperties = {}) {
        if (!papersInitialized) {
            PapersDocument.init();
            papersInitialized = true;
        }
        GObject.type_ensure(PapersView.View);
        // Papers 50: these are template objects now, not created by View
        GObject.type_ensure(PapersView.DocumentModel);
        GObject.type_ensure(PapersView.AnnotationModel);
        GObject.type_ensure(PapersView.UndoContext);
        GObject.type_ensure(PapersView.AnnotationsContext);
        GObject.type_ensure(PapersView.SearchContext);

        super(constructProperties);

        // Papers 50: View takes model and contexts through setters, not properties
        this._view.set_model(this._model);
        this._view.set_annotations_context(this._annotationsContext);
        this._view.set_search_context(this._searchContext);

        if (contentTypes.includes(fileInfo.get_content_type()))
            this._loadFile(file);
        else if (Klass.convertLibreoffice)
            Klass.convertLibreoffice(this, file);
        else
            this.markFailed(new GLib.Error('Unhandled document type'));

        setupActions(this, 'pdf', [
            ['copy', () => this._view.copy()],
        ]);

        this.markInitialized();
    }

    stop() {
        this.#loadJobId.disconnect();
        this._job?.cancel();
        this._job = null;
    }

    get toolbar() {
        return this._toolbar;
    }

    // Papers and the Office converters only read local files, and GVfs maps
    // even its FUSE paths back to archive://, smb://, sftp://: use a copy.
    // shortcut: copies stay in ~/.cache/sushi, overwritten by name; add cleanup if the cache grows.
    withLocalCopy(file, onReady) {
        const dir = GLib.build_filenamev([GLib.get_user_cache_dir(), 'sushi']);
        GLib.mkdir_with_parents(dir, 0o700);
        const copy = Gio.File.new_for_path(GLib.build_filenamev([dir, file.get_basename()]));
        file.copy_async(copy, Gio.FileCopyFlags.OVERWRITE, GLib.PRIORITY_DEFAULT,
            this.cancellable, null, (source, res) => {
                try {
                    source.copy_finish(res);
                    onReady(copy);
                } catch (error) {
                    this.markFailed(error);
                }
            });
    }

    _loadFile(file) {
        if (!file.is_native()) {
            this.withLocalCopy(file, copy => this._loadFile(copy));
            return;
        }

        this._job = PapersView.JobLoad.new();
        this._job.set_uri(file.get_uri());

        this.#loadJobId.connect(this._job);
        this._job.scheduler_push_job(PapersView.JobPriority.PRIORITY_NONE);
    }

    _onLoadJobFinished(job) {
        this.#loadJobId.disconnect();
        this._job = null;

        if (this.cancellable.is_cancelled())
            return;

        const document = job.get_loaded_document();
        try {
            // the original C function has an out param for the error
            // which gets converted to an exception by GJS.
            job.is_succeeded();
        } catch (error) {
            this.markFailed(error);
            return;
        }

        if (document.get_n_pages() === 0) {
            this.markFailed(_('No pages found in document'));
            return;
        }

        this._model.set_document(document);

        this._updatePageLabel(this._model);
        this.markReady();
    }

    _goNextPage() {
        this._view.next_page();
    }

    _goPreviousPage() {
        this._view.previous_page();
    }

    _updatePageLabel(model) {
        const currentPage = model.get_page();
        const totalPages = model.document.get_n_pages();

        this._toolbarBack.set_sensitive(currentPage > 0);
        this._toolbarForward.set_sensitive(currentPage < totalPages - 1);
        this._pageLabel.set_text(Format.vprintf(_('%d of %d'), [currentPage + 1, totalPages]));
        this._pageLabel.set_visible(true);
    }
};

const getPaperTypes = () => {
    const appInfo = GioUnix.DesktopAppInfo.new('org.gnome.Papers.desktop');
    // Papers might not be installed, fallback to only PDF
    let contentTypes = appInfo?.get_supported_types() ?? ['application/pdf'];

    const TIFF_CONTENT_TYPE = 'image/tiff';
    if (Image.contentTypes.includes(TIFF_CONTENT_TYPE))
        contentTypes = contentTypes.filter(t => t !== TIFF_CONTENT_TYPE);

    return contentTypes;
};

export const contentTypes = getPaperTypes();
