/* -*- js-indent-level: 8 -*- */
/*
 * L.Control.JSDialog
 */

/* global Hammer */
L.Control.JSDialog = L.Control.extend({
	dialogs: {},
	draggingObject: null,

	onAdd: function (map) {
		this.map = map;

		this.map.on('jsdialog', this.onJSDialog, this);
		this.map.on('jsdialogupdate', this.onJSUpdate, this);
		this.map.on('jsdialogaction', this.onJSAction, this);
	},

	onRemove: function() {
		this.map.off('jsdialog', this.onJSDialog, this);
		this.map.off('jsdialogupdate', this.onJSUpdate, this);
		this.map.off('jsdialogaction', this.onJSAction, this);
	},

	hasDialogOpened: function() {
		return Object.keys(this.dialogs).length > 0;
	},

	closeDialog: function(id) {
		var builder = this.dialogs[id].builder;
		L.DomUtil.remove(this.dialogs[id].container);
		delete this.dialogs[id];
		builder.callback('dialog', 'close', {id: '__DIALOG__'}, null, builder);
	},

	onJSDialog: function(e) {
		var posX = 0;
		var posY = 0;
		var data = e.data;

		if (this.dialogs[data.id]) {
			posX = this.dialogs[data.id].startX;
			posY = this.dialogs[data.id].startY;
			L.DomUtil.remove(this.dialogs[data.id].container);
		}

		if (data.action === 'close')
		{
			if (data.id && this.dialogs[data.id]) {
				L.DomUtil.remove(this.dialogs[data.id].container);
				delete this.dialogs[data.id];
			}
			return;
		}

		var container = L.DomUtil.create('div', 'jsdialog-container ui-dialog ui-widget-content lokdialog_container', document.body);
		container.id = data.id;
		if (data.collapsed && (data.collapsed === 'true' || data.collapsed === true))
			L.DomUtil.addClass(container, 'collapsed');

		var titlebar = L.DomUtil.create('div', 'ui-dialog-titlebar ui-corner-all ui-widget-header ui-helper-clearfix', container);
		var title = L.DomUtil.create('span', 'ui-dialog-title', titlebar);
		title.innerText = data.title;
		var button = L.DomUtil.create('button', 'ui-button ui-corner-all ui-widget ui-button-icon-only ui-dialog-titlebar-close', titlebar);
		L.DomUtil.create('span', 'ui-button-icon ui-icon ui-icon-closethick', button);

		var content = L.DomUtil.create('div', 'lokdialog ui-dialog-content ui-widget-content', container);

		var builder = new L.control.jsDialogBuilder({windowId: data.id, mobileWizard: this, map: this.map, cssClass: 'jsdialog'});
		builder.build(content, [data]);

		// We show some dialogs such as Macro Security Warning Dialog and Text Import Dialog (csv)
		// They are displayed before the document is loaded
		// Spinning should be happening until the 1st interaction with the user
		// which is the dialog opening in this case
		this.map._progressBar.end();

		var that = this;
		button.onclick = function() {
			that.closeDialog(data.id);
		};

		var onInput = function(ev) {
			if (ev.isFirst)
				that.draggingObject = that.dialogs[data.id];

			if (ev.isFinal && that.draggingObject
				&& that.draggingObject.translateX
				&& that.draggingObject.translateY) {
				that.draggingObject.startX = that.draggingObject.translateX;
				that.draggingObject.startY = that.draggingObject.translateY;
				that.draggingObject.translateX = 0;
				that.draggingObject.translateY = 0;
				that.draggingObject = null;
			}
		};

		var hammerTitlebar = new Hammer(titlebar);
		hammerTitlebar.add(new Hammer.Pan({ threshold: 20, pointers: 0 }));

		hammerTitlebar.on('panstart', this.onPan.bind(this));
		hammerTitlebar.on('panmove', this.onPan.bind(this));
		hammerTitlebar.on('hammer.input', onInput);

		if (posX === 0 && posY === 0) {
			posX = window.innerWidth/2 - container.offsetWidth/2;
			posY = window.innerHeight/2 - container.offsetHeight/2;
		}

		this.dialogs[data.id] = {
			container: container,
			builder: builder,
			startX: posX,
			startY: posY
		};

		this.updatePosition(container, posX, posY);
	},

	onJSUpdate: function (e) {
		var data = e.data;

		if (data.jsontype !== 'dialog')
			return;

		var dialog = this.dialogs[data.id] ? this.dialogs[data.id].container : null;
		if (!dialog)
			return;

		var control = dialog.querySelector('#' + data.control.id);
		if (!control) {
			console.warn('jsdialogupdate: not found control with id: "' + data.control.id + '"');
			return;
		}

		var parent = control.parentNode;
		if (!parent)
			return;

		var scrollTop = control.scrollTop;

		control.style.visibility = 'hidden';
		var builder = new L.control.jsDialogBuilder({windowId: data.id,
			mobileWizard: this,
			map: this.map,
			cssClass: 'jsdialog'});

		var temporaryParent = L.DomUtil.create('div');
		builder.build(temporaryParent, [data.control], false);
		parent.insertBefore(temporaryParent.firstChild, control.nextSibling);
		L.DomUtil.remove(control);

		var newControl = dialog.querySelector('#' + data.control.id);
		newControl.scrollTop = scrollTop;
	},

	onJSAction: function (e) {
		var data = e.data;

		if (data.jsontype !== 'dialog')
			return;

		var builder = this.dialogs[data.id] ? this.dialogs[data.id].builder : null;
		if (!builder)
			return;

		var dialog = this.dialogs[data.id] ? this.dialogs[data.id].container : null;
		if (!dialog)
			return;

		builder.executeAction(dialog, data.data);
	},

	onPan: function (ev) {
		var target = this.draggingObject;
		if (target) {
			var startX = target.startX ? target.startX : 0;
			var startY = target.startY ? target.startY : 0;

			var newX = startX + ev.deltaX;
			var newY = startY + ev.deltaY;

			// Don't allow to put dialog outside the view
			if (!(newX < 0 || newY < 0
				|| newX > window.innerWidth - target.offsetWidth/2
				|| newY > window.innerHeight - target.offsetHeight/2)) {
				target.translateX = newX;
				target.translateY = newY;

				this.updatePosition(target.container, newX, newY);
			}
		}
	},

	updatePosition: function (target, newX, newY) {
		target.style.marginLeft = newX + 'px';
		target.style.marginTop = newY + 'px';
	},

	handleKeyEvent: function (event) {
		var keyCode = event.keyCode;

		switch (keyCode) {
		case 27:
			// ESC
			var dialogs = Object.keys(this.dialogs);
			if (dialogs.length) {
				var lastKey = dialogs[dialogs.length - 1];
				this.closeDialog(lastKey);
				this.map.focus();
				return true;
			}
		}

		return false;
	}
});

L.control.jsDialog = function (options) {
	return new L.Control.JSDialog(options);
};
