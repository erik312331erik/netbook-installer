This is a rudimentary document explaining the basic usage of NetbookInstaller and NetbookBootMaker, It assumes that you are familiar with OS X.

## Contents ##


# Requirements #

  * An account with Admin privileges on OS X 10.5.x or 10.6.x - Used to prepare the Installation Dongle
  * 10.5.x Retail DVD - **not a system specific build** (i.e those bundled with a machine) - To create the Installation Dongle.
  * >=8GB External USB Disk (can be a dongle or a HDD) - This will be the Installation Dongle
  * One of our [Supported\_Devices](Supported_Devices.md) - To install OS X onto :)

# NetbookBootMaker #

NetbookBootMaker makes a USB drive with the Mac OS X Installation DVD imaged on it bootable on your netbook.

First, using Disk Utility, format your USB drive so that it has an MBR and so that it has one partition of at least 8Gb. After this, restore the 10.5 Retail DVD onto your USB stick.

Download and unzip NetbookBootMaker. Run it and set the destination to your USB stick you just restored the OS X Install DVD onto.

![http://www.jpiper.net/nbm/1.jpg](http://www.jpiper.net/nbm/1.jpg)

Click "Prepare Boot Drive", enter your password and wait for NetbookBootMaker to complete.

At this point, it is advisable to bundle the latest combo system update onto your USB Installation dongle (If there's room). As of writing this was [OS X 10.5.8](http://support.apple.com/downloads/Mac_OS_X_10_5_8_Combo_Update) (760mb) - this is purely for convenience, and can always be put on a separate USB stick or installed onto the netbook using another method.

You can now eject your USB Installation Dongle, plug it into your Netbook and boot the OS X Installer from it - don't forget to customise your install to remove extra languages and print drivers you won't be needing.

# After Install #

After you've installed OS X, you should be able to boot directly from your HDD/SDD, at which point you'll be greeted with the install wizard. After this, run the 10.5.8 combo updater mentioned in the previous paragraph. After running this, when your Netbook restarts, you'll need to boot into safe mode (hit any key during the blue progress bar on startup, type -x into the prompt and hit enter) and run NetbookInstaller (see Below) to finish your installation.

<br>
<h1>NetbookInstaller</h1>

NetbookInstaller prepares your machine to run OS X as flawlessly as possible by the way of kernel extensions, and other known fixes for specific machines.<br>
<br>
<img src='http://www.jpiper.net/nbm/2.jpg' />

You can either run it from your Netbook, or use it to patch a mounted Netbook drive (i.e a RunCore USB Interface)<br>
<br>
<table><thead><th> <b>Preset</b> </th><th> <b>Mandatory for functioning installation</b> </th><th> <b>Comments</b> </th></thead><tbody>
<tr><td> Install Bootloader </td><td> Yes (PC EFI v9 / Chameleon 2 RC2) </td><td> Choice is up to you, you can always switch at a later point </td></tr>
<tr><td> Install xx Extensions </td><td> Yes </td><td> Installs kernel extensions to support your machine </td></tr>
<tr><td> Hide NetbookInstaller Files </td><td> No </td><td> Makes for a prettier installation <sup>1</sup> </td></tr>
<tr><td> Generate a system specific dsdt.aml file </td><td> Yes </td><td>  </td></tr>
<tr><td> Enable Remote CD </td><td> No </td><td>  </td></tr>
<tr><td> Enable Hibernation </td><td> No </td><td> Experimental support, generally not advisable to enable </td></tr>
<tr><td> Enable Quiet Boot </td><td> No </td><td> It is best not to enable this so it is easier to boot into verbose and safe mode </td></tr>
<tr><td> Fix Bluetooth </td><td> No </td><td>  </td></tr></tbody></table>

<sup>1</sup> When you hide the files, you can still access UpdateExtra etc, by using  "Go -> Go to Folder" in the finder and typing "/Extra" without the quotation marks.<br>
<br>
<br>
<h1>Slimming OS X</h1>

If you've got a small (<=16GB) hard disk, you'll find OS X a bit of a squeeze. <a href='http://www.xslimmer.com/'>XSlimmer</a> can remove PPC code and extra languages from apps to slim your install down - it keeps a blacklist of files so it won't slim any system or developer apps, or those that it know will be broken by slimming.<br>
<br>
You may also want to delete the Fancy OS X Text to Speech Reader - Alex. He can take up to 600mb(!) and after deleting, you'll still have the voices that came with 10.4 - just run the following command from the terminal.<br>
<br>
<pre><code>sudo rm -rf /System/Library/Speech/Voices/Alex.SpeechVoice<br>
</code></pre>