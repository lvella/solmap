#!/usr/bin/env python3

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk
import fnmatch

class Handler:
    def __init__(self, app):
        self.app = app

    def choose_work_directory(self, *args):
        self.app.fill_entry_from_file_dialog(
            self.app.work_directory,
            "Choose intermediary output directory",
            Gtk.FileChooserAction.CREATE_FOLDER)

    def choose_output_file(self, *args):
        vtk_pattern = '*.vtk'

        ffilter = Gtk.FileFilter()
        ffilter.set_name('VTK Files')
        ffilter.add_pattern(vtk_pattern)

        fname = self.app.fill_entry_from_file_dialog(
            self.app.output_file,
            "Choose output file",
            Gtk.FileChooserAction.SAVE,
            ffilter
        )
        if fname and not fnmatch.fnmatch(fname, vtk_pattern):
            self.app.output_file.set_text(fname + '.vtk')

    def quit(self, *args):
        Gtk.main_quit()

    def process(self):
        pass

class Application:
    def __init__(self):
        builder = Gtk.Builder()
        builder.add_from_file('resources/gui.glade')
        builder.connect_signals(Handler(self))

        self.input_directory = builder.get_object('input_directory')
        self.work_directory = builder.get_object('work_directory')
        self.output_file = builder.get_object('output_file')

        self.window = builder.get_object('initial_window')
        self.window.show_all()

    def validade_input(self):
        # TODO: test if input is valid and enable execute button
        pass

    def fill_entry_from_file_dialog(self, entry, title, action, ffilter=None):
        dialog = Gtk.FileChooserDialog(title,
            self.window, action,
            (Gtk.STOCK_CANCEL, Gtk.ResponseType.CANCEL,
             Gtk.STOCK_OK, Gtk.ResponseType.OK))
        if ffilter:
            dialog.add_filter(ffilter)
        resp = dialog.run()
        if resp == Gtk.ResponseType.OK:
            fname = dialog.get_filename()
            entry.set_text(fname)
            self.validade_input()
        else:
            fname = None
        dialog.destroy()
        return fname

if __name__ == '__main__':
    a = Application()
    Gtk.main()
