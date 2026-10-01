struct Weapon;
struct Ped { Weapon* sel; Weapon* weapon; int ammo; void Copy(Ped* other); };
struct Group { Ped* leader; Ped* list[8]; int count; void Swap(int idx); };

void Group::Swap(int idx)
{
    Weapon* memberWeapon2 = list[idx]->weapon;
    Weapon* memberWeapon = list[idx]->sel;
    Weapon* leaderWeapon = leader->sel;
    Weapon* leaderWeapon2 = leader->weapon;

    leader->Copy(list[idx]);
    leader->sel = memberWeapon;
    leader->weapon = memberWeapon2;
    list[idx]->sel = leaderWeapon;
    list[idx]->weapon = leaderWeapon2;

    if (idx >= count - 1)
    {
        list[idx]->ammo = 0;
        count--;
    }
    else
    {
        list[idx]->ammo = 99;
    }
}
